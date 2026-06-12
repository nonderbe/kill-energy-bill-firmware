// Kill Energy Bill — Captive portal for AP mode
//
// Four layers — identical to the original Arduino firmware:
//
//  1. DNS server (UDP/53)    : responds to all A queries with 192.168.4.1
//                              so that connectivity-check hostnames resolve to us
//  2. TCP/443 intercept      : reads TLS ClientHello, responds with plain HTTP 302
//                              (Android issues HTTPS probes we can't terminate with TLS)
//  3. HTTP probe handlers    : /generate_204, /.well-known/captive-portal, Apple/Microsoft
//  4. Root /                 : already served by OpenBK's own WiFi setup wizard — free
//
// The DNS server and TCP/443 tasks run as permanent background tasks.
// They are harmless once the device has joined a network: new clients can't reach the
// AP-side 192.168.4.1 from the STA side, so the tasks idle with no work to do.
//
// IMPORTANT: OpenBK's HTTP server uses prefix-match first-match-wins routing.
// Register the most specific paths FIRST (see CLAUDE.md "OpenBK HTTP-routing" bug note).

#include "../new_common.h"
#include "../logging/logging.h"
#include "../httpserver/new_http.h"
#include "../pal/keb_pal.h"
#include "drv_killbill_portal.h"

// Portal is only meaningful on platforms with a network stack
#if ENABLE_SEND_POSTANDGET && !WINDOWS

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <string.h>
#include <stdlib.h>

#define PORTAL_AP_IP_STR  "192.168.4.1"
#define PORTAL_AP_IP_0    192
#define PORTAL_AP_IP_1    168
#define PORTAL_AP_IP_2    4
#define PORTAL_AP_IP_3    1

// ---- DNS server (port 53) --------------------------------------------------
//
// Reads any DNS query and responds with a single A record pointing at the AP IP.
// For AAAA (IPv6) queries we return NOERROR with 0 answers — this causes the
// resolver to fall back to an A query on the next attempt.

#define DNS_QTYPE_A     1
#define DNS_QTYPE_AAAA  28
#define DNS_QTYPE_ANY   255

// Build an authoritative DNS response.
// Returns the response length, or 0 if the query is too short/malformed.
static int portal_dns_build_response(const uint8_t *q, int qlen, uint8_t *r, int rcap) {
    if (qlen < 12 || rcap < qlen + 16) return 0;

    // Copy question verbatim; we will fixup the header bytes.
    memcpy(r, q, qlen);

    // Header: QR=1 (response), AA=1 (authoritative), RD copied from query, RA=0, RCODE=0
    r[2] = (uint8_t)(0x84 | (q[2] & 0x01)); // QR|OPCODE=0|AA|TC=0|RD
    r[3] = 0x00;                             // RA=0, Z=0, RCODE=NOERROR

    // Find QTYPE by walking the QNAME labels starting at byte 12
    const uint8_t *qname = q + 12;
    const uint8_t *end   = q + qlen;
    while (qname < end && *qname != 0) {
        if ((*qname & 0xC0) == 0xC0) { qname += 2; break; } // pointer — skip
        qname += 1 + *qname;
    }
    if (qname < end && *qname == 0) qname++; // skip null label
    uint16_t qtype = (qname + 1 < end) ? (uint16_t)((qname[0] << 8) | qname[1]) : 0;

    // For AAAA: return NOERROR with 0 answers (client will retry with A)
    if (qtype == DNS_QTYPE_AAAA) {
        r[6] = 0x00; r[7] = 0x00; // ANCOUNT = 0
        return qlen;
    }

    // For A or ANY: append a single A record answer
    r[6] = 0x00; r[7] = 0x01; // ANCOUNT = 1

    int pos = qlen;
    r[pos++] = 0xC0; r[pos++] = 0x0C; // NAME: pointer to QNAME at offset 12
    r[pos++] = 0x00; r[pos++] = 0x01; // TYPE: A
    r[pos++] = 0x00; r[pos++] = 0x01; // CLASS: IN
    r[pos++] = 0x00; r[pos++] = 0x00; // TTL high
    r[pos++] = 0x00; r[pos++] = 0x01; // TTL low = 1 second (no caching)
    r[pos++] = 0x00; r[pos++] = 0x04; // RDLENGTH: 4 bytes
    r[pos++] = PORTAL_AP_IP_0;
    r[pos++] = PORTAL_AP_IP_1;
    r[pos++] = PORTAL_AP_IP_2;
    r[pos++] = PORTAL_AP_IP_3;

    return pos;
}

static void portal_dns_task(void *arg) {
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        addLogAdv(LOG_ERROR, LOG_FEATURE_DRV, "[PORTAL] DNS: socket() failed");
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(53);

    // Retry bind — the AP interface may take a moment to come up
    int attempts = 0;
    while (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        if (++attempts >= 20) {
            addLogAdv(LOG_ERROR, LOG_FEATURE_DRV, "[PORTAL] DNS: bind port 53 failed after retries");
            close(sock);
            return;
        }
        rtos_delay_milliseconds(500);
    }
    addLogAdv(LOG_INFO, LOG_FEATURE_DRV, "[PORTAL] DNS server listening on port 53");

    static uint8_t buf[512], resp[512];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (n < 12) continue; // too short to be valid DNS

        int rlen = portal_dns_build_response(buf, n, resp, sizeof(resp));
        if (rlen > 0) {
            sendto(sock, resp, rlen, 0, (struct sockaddr *)&client, clen);
        }
    }
}

// ---- TCP/443 intercept -----------------------------------------------------
//
// Android (and some iOS flows) send HTTPS connectivity probes to the AP.
// We can't terminate TLS, so we accept the connection, drain the ClientHello,
// and respond with a plain HTTP 302 redirect pointing to the portal.
// Most captive-portal mini-browsers handle this correctly.

static const char TCP443_REDIRECT[] =
    "HTTP/1.1 302 Found\r\n"
    "Location: http://" PORTAL_AP_IP_STR "/\r\n"
    "Cache-Control: no-store, no-cache\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static void portal_tcp443_task(void *arg) {
    (void)arg;

    int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server < 0) {
        addLogAdv(LOG_ERROR, LOG_FEATURE_DRV, "[PORTAL] TCP443: socket() failed");
        return;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(443);

    int attempts = 0;
    while (bind(server, (struct sockaddr *)&local, sizeof(local)) < 0) {
        if (++attempts >= 20) {
            addLogAdv(LOG_ERROR, LOG_FEATURE_DRV, "[PORTAL] TCP443: bind port 443 failed");
            close(server);
            return;
        }
        rtos_delay_milliseconds(500);
    }
    listen(server, 4);
    addLogAdv(LOG_INFO, LOG_FEATURE_DRV, "[PORTAL] TCP/443 intercept listening");

    while (1) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            rtos_delay_milliseconds(10);
            continue;
        }
        // Short receive timeout so we don't block forever on a slow client
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Drain incoming bytes (TLS ClientHello) — we don't care about the content
        uint8_t tmp[256];
        recv(client, tmp, sizeof(tmp), 0);

        // Reply with plain-text HTTP redirect (no TLS)
        send(client, TCP443_REDIRECT, sizeof(TCP443_REDIRECT) - 1, 0);
        close(client);
    }
}

// ---- HTTP probe handlers ---------------------------------------------------
//
// IMPORTANT: poststr(request, NULL) signals end-of-response. DO NOT call
// http_setup() for 302 responses — http_setup() hardcodes " OK" after the
// status code, making "HTTP/1.1 302 OK" which breaks redirect detection.
// Write the full raw header with poststr() instead.

static const char REDIRECT_RESPONSE[] =
    "HTTP/1.1 302 Found\r\n"
    "Location: http://" PORTAL_AP_IP_STR "/\r\n"
    "Cache-Control: no-store, no-cache\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

// Android: GET /generate_204 → expect HTTP 204 or 302
static int portal_generate_204(http_request_t *request) {
    poststr(request, REDIRECT_RESPONSE);
    poststr(request, NULL);
    return 0;
}

// RFC 8908 captive-portal JSON descriptor (Android 11+)
// NOTE: user-portal-url must return content directly (not a 302) for Android
// mini-browser. OpenBK's root / serves the WiFi setup wizard, so this is fine.
static const char CAPTIVE_PORTAL_JSON[] =
    "{\"captive\":true,"
    "\"user-portal-url\":\"http://" PORTAL_AP_IP_STR "/\","
    "\"can-extend-session\":false}";

static int portal_captive_json(http_request_t *request) {
    poststr(request,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/captive+json\r\n"
        "Cache-Control: private, no-store, max-age=0\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n");
    poststr(request, CAPTIVE_PORTAL_JSON);
    poststr(request, NULL);
    return 0;
}

// HTML page for OS-level mini-browsers (Apple CNA, legacy Android).
// A meta-refresh is the safest redirect for in-app browsers that don't follow 302.
static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='0;url=http://" PORTAL_AP_IP_STR "/'>"
    "<title>&#9889; Kill Energy Bill</title>"
    "</head><body>"
    "<p>Doorverwijzen naar het configuratiescherm..."
    " <a href='http://" PORTAL_AP_IP_STR "/'>Klik hier</a></p>"
    "</body></html>";

static int portal_page(http_request_t *request) {
    http_setup(request, httpMimeTypeHTML);
    poststr(request, PORTAL_HTML);
    poststr(request, NULL);
    return 0;
}

// Apple iOS/macOS: GET /hotspot-detect.html and /library/test/success.html
// iOS CNA expects content — a 302 from these paths shows a blank mini-browser.
// Returning the portal HTML with meta-refresh gives the user a tap target.
static int portal_apple(http_request_t *request) {
    return portal_page(request);
}

// Windows NCSI: expects "Microsoft NCSI" as body with HTTP 200
static int portal_ncsi(http_request_t *request) {
    http_setup(request, httpMimeTypeText);
    poststr(request, "Microsoft NCSI");
    poststr(request, NULL);
    return 0;
}

// Windows connecttest: expects "Microsoft Connect Test" with HTTP 200
static int portal_connecttest(http_request_t *request) {
    http_setup(request, httpMimeTypeText);
    poststr(request, "Microsoft Connect Test");
    poststr(request, NULL);
    return 0;
}

// Generic redirect for any remaining OS-specific probe paths
static int portal_redirect(http_request_t *request) {
    poststr(request, REDIRECT_RESPONSE);
    poststr(request, NULL);
    return 0;
}

// ---- Init ------------------------------------------------------------------

void KillBill_Portal_Init(void) {
    // Start DNS server and TCP/443 intercept as permanent background tasks.
    // These tasks never return (infinite accept/recv loops), so keb_run_in_background's
    // automatic rtos_delete_thread after the function returns is never reached — that's fine.
    keb_run_in_background("keb_dns53", portal_dns_task, NULL);
    keb_run_in_background("keb_tcp443", portal_tcp443_task, NULL);

    // Register HTTP probe routes, most specific first (prefix-match, first-match-wins).
    // Android
    HTTP_RegisterCallback("/generate_204",    HTTP_GET, portal_generate_204, 0);
    HTTP_RegisterCallback("/gen_204",         HTTP_GET, portal_generate_204, 0);
    // RFC 8910 JSON descriptor (Android 11+, Captive Network Assistant)
    HTTP_RegisterCallback("/.well-known/captive-portal", HTTP_GET, portal_captive_json, 0);
    // Apple iOS/macOS Captive Network Assistant
    HTTP_RegisterCallback("/library/test/success.html",  HTTP_GET, portal_apple, 0);
    HTTP_RegisterCallback("/hotspot-detect.html",        HTTP_GET, portal_apple, 0);
    HTTP_RegisterCallback("/success.txt",                HTTP_GET, portal_apple, 0);
    HTTP_RegisterCallback("/canonical.html",             HTTP_GET, portal_page,  0);
    // Windows NCSI
    HTTP_RegisterCallback("/msftconnecttest/connecttest.txt", HTTP_GET, portal_connecttest, 0);
    HTTP_RegisterCallback("/connecttest.txt",            HTTP_GET, portal_connecttest, 0);
    HTTP_RegisterCallback("/ncsi.txt",                   HTTP_GET, portal_ncsi, 0);
    // Generic OS redirects
    HTTP_RegisterCallback("/redirect",                   HTTP_GET, portal_redirect, 0);
    HTTP_RegisterCallback("/fwlink/",                    HTTP_GET, portal_redirect, 0);

    addLogAdv(LOG_INFO, LOG_FEATURE_DRV, "[PORTAL] captive portal handlers registered");
}

#else // !ENABLE_SEND_POSTANDGET || WINDOWS

void KillBill_Portal_Init(void) {}

#endif
