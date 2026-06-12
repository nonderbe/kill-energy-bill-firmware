// Kill Energy Bill — DNS-SD P1 meter discovery (mDNS PTR query over UDP)
//
// Uses a raw UDP socket with the QU (unicast-response) bit set in the DNS question.
// Responses arrive as unicast on our ephemeral source port, so there is no conflict
// with the existing lwIP mDNS responder that holds port 5353.
//
// Layer 1: _hwenergy._tcp.local  (HomeWizard Wi-Fi P1 meter)
// Layer 2: _esphome._tcp.local + HTTP probe for "active_power_w"  (Slimmelezer / ESPHome)
//
// This file compiles only when ENABLE_SEND_POSTANDGET is set (lwIP sockets available).

#include "../new_common.h"
#include "keb_mdns_client.h"

#if ENABLE_SEND_POSTANDGET && !WINDOWS

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define MDNS_GROUP        "224.0.0.251"
#define MDNS_PORT         5353
#define MDNS_TIMEOUT_S    2
#define MDNS_BUF          512

#define DNS_TYPE_A        1
#define DNS_TYPE_PTR      12
#define DNS_TYPE_SRV      33
#define DNS_CLASS_QU      0x8001   // IN + unicast-response bit

// ---- DNS wire-format helpers -----------------------------------------------

// Encode a dotted name ("foo.bar.local") into DNS label format.
// Returns bytes written (including root \0), or 0 on overflow.
static int dns_encode_name(uint8_t *out, int cap, const char *name) {
    int pos = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        int n = dot ? (int)(dot - name) : (int)strlen(name);
        if (n == 0 || pos + 1 + n + 1 >= cap) return 0;
        out[pos++] = (uint8_t)n;
        memcpy(out + pos, name, n);
        pos += n;
        if (dot) name = dot + 1; else break;
    }
    if (pos >= cap) return 0;
    out[pos++] = 0;
    return pos;
}

// Decode a DNS name at byte offset *pos_inout, following pointer compression.
// Updates *pos_inout past the name (or past the first pointer, not the target).
// Returns number of bytes consumed at the original offset (to advance the caller).
static int dns_decode_name(const uint8_t *pkt, int pkt_len, int pos,
                           char *out, int out_cap) {
    int out_pos = 0;
    int orig    = pos;
    int jumped  = 0;
    int limit   = 64;

    while (pos < pkt_len && limit-- > 0) {
        uint8_t b = pkt[pos];
        if (b == 0) {
            if (!jumped) return pos - orig + 1;
            return 0; // consumed already accounted for
        }
        if ((b & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt_len) break;
            int ret = jumped ? 0 : pos - orig + 2;
            pos = ((b & 0x3F) << 8) | pkt[pos + 1];
            jumped = 1;
            if (ret) { limit++; continue; } // ret already set, keep consuming
            // After first jump: caller has consumed 2 bytes at orig, we now decode
            // the target in place — no extra consumed bytes.
            continue;
        }
        pos++;
        int n = b;
        if (out_pos + n + 2 < out_cap) {
            if (out_pos) out[out_pos++] = '.';
            memcpy(out + out_pos, pkt + pos, n);
            out_pos += n;
        }
        pos += n;
    }
    if (out_pos < out_cap) out[out_pos] = '\0';
    // If we jumped, the original bytes consumed = 2 (pointer); non-jumped handled above.
    return jumped ? (/* unreachable for correct callers */0) : (pos - orig + 1);
}

// Build a DNS PTR query with the QU bit requesting unicast response.
static int build_ptr_query(uint8_t *buf, int cap, const char *service) {
    if (cap < 12) return 0;
    buf[0]=0;  buf[1]=1;   // ID=1
    buf[2]=0;  buf[3]=0;   // standard query
    buf[4]=0;  buf[5]=1;   // QDCOUNT=1
    buf[6]=0;  buf[7]=0;
    buf[8]=0;  buf[9]=0;
    buf[10]=0; buf[11]=0;
    int pos = 12;
    int n = dns_encode_name(buf + pos, cap - pos, service);
    if (!n) return 0;
    pos += n;
    if (pos + 4 > cap) return 0;
    buf[pos++] = 0x00; buf[pos++] = DNS_TYPE_PTR;
    buf[pos++] = (DNS_CLASS_QU >> 8) & 0xFF;
    buf[pos++] =  DNS_CLASS_QU       & 0xFF;
    return pos;
}

// Parse a DNS response packet; extract the first A record IP and any SRV port.
// Returns true if an A record was found and host_out was filled.
static bool dns_parse_response(const uint8_t *pkt, int len,
                               char *host_out, int host_cap, uint16_t *port_out) {
    if (len < 12) return false;
    int qdcount = (pkt[4]  << 8) | pkt[5];
    int ancount = (pkt[6]  << 8) | pkt[7];
    int nscount = (pkt[8]  << 8) | pkt[9];
    int arcount = (pkt[10] << 8) | pkt[11];
    int pos = 12;

    // Skip question section
    for (int i = 0; i < qdcount && pos < len; i++) {
        char tmp[64];
        int c = dns_decode_name(pkt, len, pos, tmp, sizeof(tmp));
        if (c <= 0) return false;
        pos += c + 4; // +4 for QTYPE + QCLASS
    }

    char found_ip[20] = {0};
    uint16_t found_port = 80;
    bool has_a = false;

    int total = ancount + nscount + arcount;
    if (total > 64) total = 64;

    for (int i = 0; i < total && pos + 10 <= len; i++) {
        char rname[64];
        int c = dns_decode_name(pkt, len, pos, rname, sizeof(rname));
        if (c <= 0) break;
        pos += c;
        if (pos + 10 > len) break;

        uint16_t rtype = (pkt[pos]   << 8) | pkt[pos+1];
        uint16_t rdlen = (pkt[pos+8] << 8) | pkt[pos+9];
        pos += 10;
        if (pos + rdlen > len) break;

        if (rtype == DNS_TYPE_SRV && rdlen >= 6) {
            found_port = (pkt[pos+4] << 8) | pkt[pos+5];
            if (found_port == 0) found_port = 80;
        } else if (rtype == DNS_TYPE_A && rdlen == 4) {
            snprintf(found_ip, sizeof(found_ip), "%u.%u.%u.%u",
                     pkt[pos], pkt[pos+1], pkt[pos+2], pkt[pos+3]);
            has_a = true;
        }
        pos += rdlen;
    }

    if (has_a) {
        strncpy(host_out, found_ip, host_cap - 1);
        host_out[host_cap - 1] = '\0';
        *port_out = found_port;
        return true;
    }
    return false;
}

// ---- mDNS PTR query --------------------------------------------------------

// Sends a PTR query for `service` and waits MDNS_TIMEOUT_S seconds for a response.
// Falls back to the sender IP if no A record is in the response.
static bool mdns_query_service(const char *service,
                               char *host_out, int host_cap, uint16_t *port_out) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    // Ephemeral source port — unicast responses arrive here (QU bit)
    struct sockaddr_in local = {0};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = 0;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock);
        return false;
    }

    struct timeval tv = { .tv_sec = MDNS_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t qbuf[128];
    int qlen = build_ptr_query(qbuf, sizeof(qbuf), service);
    if (!qlen) { close(sock); return false; }

    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(MDNS_PORT);
    dst.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    sendto(sock, qbuf, qlen, 0, (struct sockaddr *)&dst, sizeof(dst));

    uint8_t rbuf[MDNS_BUF];
    bool found = false;
    while (!found) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, rbuf, sizeof(rbuf), 0,
                         (struct sockaddr *)&src, &src_len);
        if (n <= 0) break;
        uint16_t tmp_port = 80;
        if (dns_parse_response(rbuf, n, host_out, host_cap, &tmp_port)) {
            // Some responders put 0.0.0.0 in the A record — fall back to sender IP
            if (strcmp(host_out, "0.0.0.0") == 0) {
                strncpy(host_out, inet_ntoa(src.sin_addr), host_cap - 1);
                host_out[host_cap - 1] = '\0';
            }
            *port_out = tmp_port;
            found = true;
        }
    }
    close(sock);
    return found;
}

// ---- HTTP probe ------------------------------------------------------------

// Synchronous TCP GET /api/v1/data; returns true if body contains "active_power_w".
static bool http_probe_p1(const char *host, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    char req[128];
    int req_len = snprintf(req, sizeof(req),
        "GET /api/v1/data HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    send(sock, req, req_len, 0);

    char resp[512];
    int total = 0, n;
    while ((n = recv(sock, resp + total, (int)sizeof(resp) - total - 1, 0)) > 0) {
        total += n;
        if (total >= (int)sizeof(resp) - 1) break;
    }
    resp[total] = '\0';
    close(sock);
    return strstr(resp, "active_power_w") != NULL;
}

// ---- Hostname conflict probe -----------------------------------------------

// Build a DNS A query with the QU bit for `hostname.local`.
static int build_a_query(uint8_t *buf, int cap, const char *hostname) {
    if (cap < 12) return 0;
    buf[0]=0;  buf[1]=2;   // ID=2 (distinct from PTR query ID=1)
    buf[2]=0;  buf[3]=0;
    buf[4]=0;  buf[5]=1;   // QDCOUNT=1
    buf[6]=0;  buf[7]=0;
    buf[8]=0;  buf[9]=0;
    buf[10]=0; buf[11]=0;
    char fqdn[48];
    snprintf(fqdn, sizeof(fqdn), "%s.local", hostname);
    int pos = 12;
    int n = dns_encode_name(buf + pos, cap - pos, fqdn);
    if (!n) return 0;
    pos += n;
    if (pos + 4 > cap) return 0;
    buf[pos++] = 0x00; buf[pos++] = DNS_TYPE_A;
    buf[pos++] = (DNS_CLASS_QU >> 8) & 0xFF;
    buf[pos++] =  DNS_CLASS_QU       & 0xFF;
    return pos;
}

// Parse a DNS response for an A record; returns the IP as uint32_t (network byte
// order), or 0 if not found.
static uint32_t parse_a_record_ip(const uint8_t *pkt, int len) {
    if (len < 12) return 0;
    int qdcount = (pkt[4]  << 8) | pkt[5];
    int ancount = (pkt[6]  << 8) | pkt[7];
    int nscount = (pkt[8]  << 8) | pkt[9];
    int arcount = (pkt[10] << 8) | pkt[11];
    int pos = 12;

    for (int i = 0; i < qdcount && pos < len; i++) {
        char tmp[64];
        int c = dns_decode_name(pkt, len, pos, tmp, sizeof(tmp));
        if (c <= 0) return 0;
        pos += c + 4;
    }

    int total = ancount + nscount + arcount;
    if (total > 64) total = 64;
    for (int i = 0; i < total && pos + 10 <= len; i++) {
        char rname[64];
        int c = dns_decode_name(pkt, len, pos, rname, sizeof(rname));
        if (c <= 0) break;
        pos += c;
        if (pos + 10 > len) break;
        uint16_t rtype = (pkt[pos]   << 8) | pkt[pos+1];
        uint16_t rdlen = (pkt[pos+8] << 8) | pkt[pos+9];
        pos += 10;
        if (pos + rdlen > len) break;
        if (rtype == DNS_TYPE_A && rdlen == 4) {
            uint32_t ip;
            memcpy(&ip, pkt + pos, 4);
            return ip;
        }
        pos += rdlen;
    }
    return 0;
}

// Probe hostname.local with a 300 ms window.
// Returns true if another device (IP ≠ own_ip) holds the name.
bool keb_mdns_name_conflict(const char *hostname, uint32_t own_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    struct sockaddr_in local = {0};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = 0;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock);
        return false;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t qbuf[128];
    int qlen = build_a_query(qbuf, sizeof(qbuf), hostname);
    if (!qlen) { close(sock); return false; }

    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(MDNS_PORT);
    dst.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    sendto(sock, qbuf, qlen, 0, (struct sockaddr *)&dst, sizeof(dst));

    uint8_t rbuf[MDNS_BUF];
    bool conflict = false;
    while (!conflict) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, rbuf, sizeof(rbuf), 0,
                         (struct sockaddr *)&src, &src_len);
        if (n <= 0) break;
        uint32_t resp_ip = parse_a_record_ip(rbuf, n);
        if (resp_ip == 0) {
            // No A record in this packet — use the sender's IP as fallback
            resp_ip = src.sin_addr.s_addr;
        }
        if (resp_ip != 0 && resp_ip != own_ip) {
            conflict = true;
        }
    }
    close(sock);
    return conflict;
}

void keb_mdns_resolve_hostname(const char *base_name, uint32_t own_ip,
                               char *out, size_t out_len) {
    char candidate[32];
    // Try base_name, then base_name-1 through base_name-9
    strncpy(candidate, base_name, sizeof(candidate) - 1);
    candidate[sizeof(candidate) - 1] = '\0';

    for (int i = 0; i <= 9; i++) {
        if (!keb_mdns_name_conflict(candidate, own_ip)) {
            strncpy(out, candidate, out_len - 1);
            out[out_len - 1] = '\0';
            return;
        }
        snprintf(candidate, sizeof(candidate), "%s-%d", base_name, i + 1);
    }
    // All taken — fall back to base_name
    strncpy(out, base_name, out_len - 1);
    out[out_len - 1] = '\0';
}

// ---- Public API ------------------------------------------------------------

bool keb_mdns_discover_p1(char *host_out, size_t host_len, uint16_t *port_out) {
    uint16_t port = 80;

    // Layer 1: HomeWizard _hwenergy._tcp — no probe needed, service type is specific
    if (mdns_query_service("_hwenergy._tcp.local", host_out, (int)host_len, &port)) {
        *port_out = port;
        return true;
    }

    // Layer 2: ESPHome _esphome._tcp — verify with HTTP probe (Slimmelezer, custom)
    char tmp[64] = {0};
    if (mdns_query_service("_esphome._tcp.local", tmp, sizeof(tmp), &port)) {
        if (http_probe_p1(tmp, port)) {
            strncpy(host_out, tmp, host_len - 1);
            host_out[host_len - 1] = '\0';
            *port_out = port;
            return true;
        }
    }

    return false;
}

#else // !ENABLE_SEND_POSTANDGET || WINDOWS

bool keb_mdns_discover_p1(char *host_out, size_t host_len, uint16_t *port_out) {
    (void)host_out; (void)host_len; (void)port_out;
    return false;
}

bool keb_mdns_name_conflict(const char *hostname, uint32_t own_ip) {
    (void)hostname; (void)own_ip;
    return false;
}

void keb_mdns_resolve_hostname(const char *base_name, uint32_t own_ip,
                               char *out, size_t out_len) {
    (void)own_ip;
    strncpy(out, base_name, out_len - 1);
    out[out_len - 1] = '\0';
}

#endif
