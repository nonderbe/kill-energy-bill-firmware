// Kill Energy Bill — Ed25519 OTA driver for OpenBK (BK7231N + ESP32 ESPIDF)
//
// Uses the same /ota/check API as the ESP32 Arduino firmware:
//   GET http://firmware.local-share.com/ota/check?version=X.Y.Z&channel=bk7231n
//   Response: {"update_available":true,"version":"...","url":"...","sha256":"...","ed25519_sig":"..."}
//
// Verification:
//   1. Ed25519-verify the signed message:  sig(64) || sha256_bytes(32)  → valid?
//   2. Stream firmware binary to OTA flash, compute SHA-256 in parallel
//   3. Compare computed SHA-256 with manifest sha256
//   4. Both pass → bk_reboot(); either fails → abort, no reboot
//
// On BK7231N the bootloader switches to OTA partition only on an explicit
// reboot triggered from this code path, so not calling bk_reboot() keeps
// the current firmware active even after a partial/invalid OTA write.

#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
#include "../logging/logging.h"
#include "../pal/keb_pal.h"
#include "../cJSON/cJSON.h"
#include "../base64/base64.h"
#include "drv_killbill_ota.h"

#if ENABLE_SEND_POSTANDGET
#include "../httpclient/http_client.h"
#endif

#if PLATFORM_BEKEN
#include "../hal/hal_ota.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ============================================================================
// Configuration
// ============================================================================

#define OTA_PRIMARY_HOST   "http://firmware.kill-energy-bill.com"
#define OTA_SECONDARY_HOST "http://firmware.local-share.com"
#define OTA_CHANNEL        "bk7231n"
#define OTA_CHECK_INTERVAL_S  21600u  // 6 hours
#define OTA_RETRY_INTERVAL_S   1800u  // 30 minutes
#define OTA_INITIAL_DELAY_S      30u  // first check after boot
#define OTA_DL_BUF_SIZE          2048

// Ed25519 public key — matches OTA_SIGNING_PUBKEY in config.h
static const uint8_t KEB_OTA_PUBKEY[32] = {
    0x2a,0xab,0x67,0xef,0x03,0x79,0x06,0x7f,
    0xd2,0x3e,0x88,0xe8,0xf2,0x36,0x77,0x88,
    0xe4,0xbc,0xb7,0x02,0x86,0xd2,0x75,0xdc,
    0x37,0x3e,0x59,0xbf,0xf4,0x43,0xf9,0x9b
};

// ============================================================================
// Minimal streaming SHA-256 (FIPS 180-4)
// ============================================================================

typedef struct { uint32_t state[8]; uint8_t buf[64]; uint32_t lo, hi; } sha256_ctx_t;

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define R32(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha256_block(sha256_ctx_t *c, const uint8_t *b) {
    uint32_t w[64],a,e,t1,t2; int i;
    for(i=0;i<16;i++) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for(i=16;i<64;i++){uint32_t s0=R32(w[i-15],7)^R32(w[i-15],18)^(w[i-15]>>3);uint32_t s1=R32(w[i-2],17)^R32(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    uint32_t s[8]; for(i=0;i<8;i++) s[i]=c->state[i];
    for(i=0;i<64;i++){
        uint32_t S1=R32(s[4],6)^R32(s[4],11)^R32(s[4],25);
        uint32_t ch=(s[4]&s[5])^(~s[4]&s[6]);
        t1=s[7]+S1+ch+SHA256_K[i]+w[i];
        uint32_t S0=R32(s[0],2)^R32(s[0],13)^R32(s[0],22);
        uint32_t mj=(s[0]&s[1])^(s[0]&s[2])^(s[1]&s[2]);
        t2=S0+mj;
        s[7]=s[6];s[6]=s[5];s[5]=s[4];s[4]=s[3]+t1;s[3]=s[2];s[2]=s[1];s[1]=s[0];s[0]=t1+t2;
        (void)a; (void)e;
    }
    for(i=0;i<8;i++) c->state[i]+=s[i];
}
static void sha256_init(sha256_ctx_t *c){
    c->state[0]=0x6a09e667;c->state[1]=0xbb67ae85;c->state[2]=0x3c6ef372;c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f;c->state[5]=0x9b05688c;c->state[6]=0x1f83d9ab;c->state[7]=0x5be0cd19;
    c->lo=c->hi=0;
}
static void sha256_update(sha256_ctx_t *c, const uint8_t *d, size_t n){
    uint32_t used=(c->lo>>3)&63;
    c->lo+=(uint32_t)(n<<3); if(c->lo<(uint32_t)(n<<3))c->hi++;
    c->hi+=(uint32_t)(n>>29);
    if(used){size_t fill=64-used;if(n<fill){memcpy(c->buf+used,d,n);return;}memcpy(c->buf+used,d,fill);sha256_block(c,c->buf);d+=fill;n-=fill;}
    while(n>=64){sha256_block(c,d);d+=64;n-=64;}
    if(n)memcpy(c->buf,d,n);
}
static void sha256_final(sha256_ctx_t *c, uint8_t out[32]){
    uint32_t used=(c->lo>>3)&63; uint8_t *p=c->buf+used; *p++=0x80;
    if(used<56)memset(p,0,55-used); else{memset(p,0,63-used);sha256_block(c,c->buf);memset(c->buf,0,56);}
    uint32_t hi=c->hi,lo=c->lo;
    c->buf[56]=(hi>>24)&0xff;c->buf[57]=(hi>>16)&0xff;c->buf[58]=(hi>>8)&0xff;c->buf[59]=hi&0xff;
    c->buf[60]=(lo>>24)&0xff;c->buf[61]=(lo>>16)&0xff;c->buf[62]=(lo>>8)&0xff;c->buf[63]=lo&0xff;
    sha256_block(c,c->buf);
    for(int i=0;i<8;i++){out[i*4]=(c->state[i]>>24)&0xff;out[i*4+1]=(c->state[i]>>16)&0xff;out[i*4+2]=(c->state[i]>>8)&0xff;out[i*4+3]=c->state[i]&0xff;}
}

// ============================================================================
// TweetNaCl Ed25519 verify (public-domain verify-only subset)
// Source: tweetnacl.20140427.c — D.J.Bernstein et al.  https://tweetnacl.cr.yp.to/
// ============================================================================

typedef unsigned char      tn_u8;
typedef unsigned long long tn_u64;
typedef long long          tn_i64;
typedef tn_i64 tn_gf[16];
#define TN_FOR(i,n) for(int i=0;i<(n);++i)
#define tn_sv static void
static const tn_u64 TN_K512[80]={
  0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
  0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
  0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
  0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};
static tn_u64 tn_r64(tn_u64 x,int c){return(x>>c)|(x<<(64-c));}
static tn_u64 tn_dl64(const tn_u8*x){tn_u64 u=0;TN_FOR(i,8)u=(u<<8)|x[i];return u;}
tn_sv tn_ts64(tn_u8*x,tn_u64 u){for(int i=7;i>=0;--i){x[i]=(tn_u8)u;u>>=8;}}
static int tn_hblk(tn_u8*x,const tn_u8*m,tn_u64 n){
  tn_u64 z[8],b[8],a[8],w[16],t;
  TN_FOR(i,8)z[i]=a[i]=tn_dl64(x+8*i);
  while(n>=128){
    TN_FOR(i,16)w[i]=tn_dl64(m+8*i);
    TN_FOR(i,80){
      TN_FOR(j,8)b[j]=a[j];
      t=a[7]+(tn_r64(a[4],14)^tn_r64(a[4],18)^tn_r64(a[4],41))+((a[4]&a[5])^(~a[4]&a[6]))+TN_K512[i];
      if(i<16)t+=w[i];else{w[i%16]+=(tn_r64(w[(i-2)%16],19)^tn_r64(w[(i-2)%16],61)^(w[(i-2)%16]>>6))+w[(i-7)%16]+(tn_r64(w[(i-15)%16],1)^tn_r64(w[(i-15)%16],8)^(w[(i-15)%16]>>7));t+=w[i%16];}
      a[7]=b[6];a[6]=b[5];a[5]=b[4];a[4]=b[3]+t;a[3]=b[2];a[2]=b[1];a[1]=b[0];
      a[0]=t+(tn_r64(b[0],28)^tn_r64(b[0],34)^tn_r64(b[0],39))+((b[0]&b[1])^(b[0]&b[2])^(b[1]&b[2]));
    }
    TN_FOR(i,8){a[i]+=z[i];z[i]=a[i];}m+=128;n-=128;
  }
  TN_FOR(i,8)tn_ts64(x+8*i,z[i]);return(int)n;
}
static const tn_u8 tn_iv[64]={0x6a,0x09,0xe6,0x67,0xf3,0xbc,0xc9,0x08,0xbb,0x67,0xae,0x85,0x84,0xca,0xa7,0x3b,0x3c,0x6e,0xf3,0x72,0xfe,0x94,0xf8,0x2b,0xa5,0x4f,0xf5,0x3a,0x5f,0x1d,0x36,0xf1,0x51,0x0e,0x52,0x7f,0xad,0xe6,0x82,0xd1,0x9b,0x05,0x68,0x8c,0x2b,0x3e,0x6c,0x1f,0x1f,0x83,0xd9,0xab,0xfb,0x41,0xbd,0x6b,0x5b,0xe0,0xcd,0x19,0x13,0x7e,0x21,0x79};
static int tn_hash(tn_u8*out,const tn_u8*m,tn_u64 n){
  tn_u8 h[64],x[256];tn_u64 i,b=n;TN_FOR(i,64)h[i]=tn_iv[i];
  tn_hblk(h,m,n);m+=n;n&=127;m-=n;TN_FOR(i,256)x[i]=0;TN_FOR(i,(int)n)x[i]=m[i];x[n]=128;
  n=256-128*(n<112);x[n-9]=(tn_u8)(b>>61);tn_ts64(x+n-8,b<<3);tn_hblk(h,x,n);TN_FOR(i,64)out[i]=h[i];return 0;
}
static int tn_vn(const tn_u8*x,const tn_u8*y,int n){tn_u8 d=0;TN_FOR(i,n)d|=x[i]^y[i];return(1&((d-1)>>8))-1;}
static int tn_v32(const tn_u8*x,const tn_u8*y){return tn_vn(x,y,32);}
tn_sv tn_set(tn_gf r,const tn_gf a){TN_FOR(i,16)r[i]=a[i];}
tn_sv tn_sel(tn_gf p,tn_gf q,int b){tn_i64 t,c=~(b-1);TN_FOR(i,16){t=c&(p[i]^q[i]);p[i]^=t;q[i]^=t;}}
tn_sv tn_car(tn_gf o){tn_i64 c;TN_FOR(i,16){o[i]+=(1LL<<16);c=o[i]>>16;o[(i+1)%16]+=c-1+37*(c-1)*(i==15);o[i]-=c<<16;}}
tn_sv tn_A(tn_gf o,const tn_gf a,const tn_gf b){TN_FOR(i,16)o[i]=a[i]+b[i];}
tn_sv tn_Z(tn_gf o,const tn_gf a,const tn_gf b){TN_FOR(i,16)o[i]=a[i]-b[i];}
tn_sv tn_M(tn_gf o,const tn_gf a,const tn_gf b){tn_i64 t[31];TN_FOR(i,31)t[i]=0;TN_FOR(i,16)TN_FOR(j,16)t[i+j]+=a[i]*b[j];TN_FOR(i,15)t[i]+=38*t[i+16];TN_FOR(i,16)o[i]=t[i];tn_car(o);tn_car(o);}
tn_sv tn_S(tn_gf o,const tn_gf a){tn_M(o,a,a);}
tn_sv tn_inv(tn_gf o,const tn_gf inp){tn_gf c;TN_FOR(i,16)c[i]=inp[i];for(int a=253;a>=0;--a){tn_S(c,c);if(a!=2&&a!=4)tn_M(c,c,inp);}TN_FOR(i,16)o[i]=c[i];}
tn_sv tn_p2523(tn_gf o,const tn_gf inp){tn_gf c;TN_FOR(i,16)c[i]=inp[i];for(int a=250;a>=0;--a){tn_S(c,c);if(a!=1)tn_M(c,c,inp);}TN_FOR(i,16)o[i]=c[i];}
static const tn_gf tn_0={0},tn_1={1};
static const tn_gf tn_D ={0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203};
static const tn_gf tn_D2={0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406};
static const tn_gf tn_X ={0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169};
static const tn_gf tn_Y ={0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666};
static const tn_gf tn_I ={0xa0b0,0x4a0e,0x1b27,0xc4ee,0xe478,0xad2f,0x1806,0x2f43,0xd7a7,0x3dfb,0x0099,0x2b4d,0xdf0b,0x4fc1,0x2480,0x2b83};
tn_sv tn_pk25(tn_u8*o,const tn_gf n){int i,j,b;tn_gf m,t;tn_set(t,n);tn_car(t);tn_car(t);tn_car(t);TN_FOR(j,2){m[0]=t[0]-0xffed;for(i=1;i<15;i++){m[i]=t[i]-0xffff-((m[i-1]>>16)&1);m[i-1]&=0xffff;}m[15]=t[15]-0x7fff-((m[14]>>16)&1);b=(m[15]>>16)&1;m[14]&=0xffff;tn_sel(t,m,1-b);}TN_FOR(i,16){o[2*i]=(tn_u8)(t[i]&0xff);o[2*i+1]=(tn_u8)(t[i]>>8);}}
static int tn_par(const tn_gf a){tn_u8 d[32];tn_pk25(d,a);return d[0]&1;}
tn_sv tn_pack(tn_u8*r,tn_gf p[4]){tn_gf tx,ty,zi;tn_inv(zi,p[2]);tn_M(tx,p[0],zi);tn_M(ty,p[1],zi);tn_pk25(r,ty);r[31]^=(tn_u8)(tn_par(tx)<<7);}
static int tn_neq(const tn_gf a,const tn_gf b){tn_u8 c[32],d[32];tn_pk25(c,a);tn_pk25(d,b);return tn_v32(c,d);}
static int tn_uneg(tn_gf r[4],const tn_u8 p[32]){tn_gf t,chk,num,den,d2,d4,d6;tn_set(r[2],tn_1);TN_FOR(i,16)r[1][i]=(tn_i64)p[2*i]|((tn_i64)p[2*i+1]<<8);r[1][15]&=0x7fff;tn_S(num,r[1]);tn_M(den,num,tn_D);tn_Z(num,num,r[2]);tn_A(den,r[2],den);tn_S(d2,den);tn_S(d4,d2);tn_M(d6,d4,d2);tn_M(t,d6,num);tn_M(t,t,den);tn_p2523(t,t);tn_M(t,t,num);tn_M(t,t,den);tn_M(t,t,den);tn_M(r[0],t,den);tn_S(chk,r[0]);tn_M(chk,chk,den);if(tn_neq(chk,num))tn_M(r[0],r[0],tn_I);tn_S(chk,r[0]);tn_M(chk,chk,den);if(tn_neq(chk,num))return -1;if(tn_par(r[0])==(p[31]>>7))tn_Z(r[0],tn_0,r[0]);tn_M(r[3],r[0],r[1]);return 0;}
tn_sv tn_add(tn_gf p[4],tn_gf q[4]){tn_gf a,b,c,d,t,e,f,g,h;tn_Z(a,p[1],p[0]);tn_Z(t,q[1],q[0]);tn_M(a,a,t);tn_A(b,p[0],p[1]);tn_A(t,q[0],q[1]);tn_M(b,b,t);tn_M(c,p[3],q[3]);tn_M(c,c,tn_D2);tn_M(d,p[2],q[2]);tn_A(d,d,d);tn_Z(e,b,a);tn_Z(f,d,c);tn_A(g,d,c);tn_A(h,b,a);tn_M(p[0],e,f);tn_M(p[1],h,g);tn_M(p[2],g,f);tn_M(p[3],e,h);}
tn_sv tn_csw(tn_gf p[4],tn_gf q[4],tn_u8 b){TN_FOR(i,4)tn_sel(p[i],q[i],b);}
tn_sv tn_smul(tn_gf p[4],tn_gf q[4],const tn_u8*s){tn_set(p[0],tn_0);tn_set(p[1],tn_1);tn_set(p[2],tn_1);tn_set(p[3],tn_0);for(int i=255;i>=0;--i){tn_u8 b=(s[i/8]>>(i&7))&1;tn_csw(p,q,b);tn_add(q,p);tn_add(p,p);tn_csw(p,q,b);}}
tn_sv tn_sb(tn_gf p[4],const tn_u8*s){tn_gf q[4];tn_set(q[0],tn_X);tn_set(q[1],tn_Y);tn_set(q[2],tn_1);tn_M(q[3],tn_X,tn_Y);tn_smul(p,q,s);}
static const tn_u64 tn_L[32]={0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
tn_sv tn_modL(tn_u8*r,tn_i64 x[64]){tn_i64 carry;for(int i=63;i>=32;--i){carry=0;for(int j=i-32;j<i-12;++j){x[j]+=carry-16*x[i]*(tn_i64)tn_L[j-(i-32)];carry=(x[j]+128)>>8;x[j]-=carry<<8;}x[i-12]+=carry;x[i]=0;}carry=0;TN_FOR(j,32){x[j]+=carry-(x[31]>>4)*(tn_i64)tn_L[j];carry=x[j]>>8;x[j]&=255;}TN_FOR(j,32)x[j]-=carry*(tn_i64)tn_L[j];TN_FOR(i,32){x[i+1]+=x[i]>>8;r[i]=(tn_u8)(x[i]&255);}}
tn_sv tn_red(tn_u8*r){tn_i64 x[64];TN_FOR(i,64)x[i]=r[i];TN_FOR(i,64)r[i]=0;tn_modL(r,x);}

// Verify Ed25519 signed message: sm = sig(64) || message(n), pk = 32-byte public key.
// Returns 0 on success, -1 on failure.
static int tn_open(tn_u8*m,tn_u64*mlen,const tn_u8*sm,tn_u64 smlen,const tn_u8*pk){
  tn_u8 t[32],h[64];tn_gf p[4],q[4];
  *mlen=(tn_u64)-1;if(smlen<64)return -1;if(tn_uneg(q,pk))return -1;
  TN_FOR(i,(int)smlen)m[i]=sm[i];TN_FOR(i,32)m[i+32]=pk[i];tn_hash(h,m,smlen);tn_red(h);tn_smul(p,q,h);
  tn_sb(q,sm+32);tn_add(p,q);tn_pack(t,p);smlen-=64;
  if(tn_v32(t,sm)){TN_FOR(i,(int)smlen)m[i]=0;return -1;}
  TN_FOR(i,(int)smlen)m[i]=sm[i+64];*mlen=smlen;return 0;
}

// ============================================================================
// Verify Ed25519 signature over SHA-256 hash
// ============================================================================

static bool ota_verify(const char *sig_b64, const uint8_t sha256[32]) {
    uint8_t sig[64];
    if (!b64_decode(sig_b64, sig, sizeof(sig))) {
        keb_log("OTA", "base64 decode handtekening mislukt");
        return false;
    }
    // Signed message: sig(64) || sha256(32) = 96 bytes
    uint8_t sm[96], m[96];
    memcpy(sm,      sig,    64);
    memcpy(sm + 64, sha256, 32);
    tn_u64 mlen = 0;
    if (tn_open(m, &mlen, sm, 96, KEB_OTA_PUBKEY) != 0) {
        keb_log("OTA", "Ed25519 handtekening ongeldig");
        return false;
    }
    return true;
}

// ============================================================================
// OTA state
// ============================================================================

static bool      s_in_flight = false;  // any HTTP request in-flight
static uint32_t  s_tick_s    = 0;      // elapsed seconds in IDLE
static uint32_t  s_wait_s    = OTA_INITIAL_DELAY_S;

// Manifest data filled on a successful primary/secondary parse
static char    s_dl_url[256];
static char    s_sha256_hex[65];
static uint8_t s_sha256_expected[32];
static char    s_sig_b64[128];
static bool    s_download_pending = false;

static sha256_ctx_t s_sha256_ctx;

#if ENABLE_SEND_POSTANDGET && PLATFORM_BEKEN
static httprequest_t s_dl_req;
static char          s_dl_buf[OTA_DL_BUF_SIZE];
extern void bk_reboot(void);
extern int  init_ota(unsigned int startaddr);
extern void add_otadata(unsigned char *data, int len);
extern void close_ota(void);
#endif

// ============================================================================
// Version comparison
// ============================================================================

static bool ota_is_newer(const char *candidate, const char *current) {
    int ca=0,cb=0,cc=0,da=0,db=0,dc=0;
    if (sscanf(candidate,"%d.%d.%d",&ca,&cb,&cc)!=3) return false;
    if (sscanf(current,  "%d.%d.%d",&da,&db,&dc)!=3) return false;
    if (ca!=da) return ca>da; if (cb!=db) return cb>db; return cc>dc;
}

// ============================================================================
// Manifest response handler (shared by primary + secondary)
// ============================================================================

static void on_manifest_ok(const char *body) {
    cJSON *doc = cJSON_Parse(body);
    if (!doc) { keb_log("OTA","manifest JSON fout"); return; }

    // update_available=false → up to date, no further action
    cJSON *j_avail = cJSON_GetObjectItem(doc,"update_available");
    if (cJSON_IsBool(j_avail) && !cJSON_IsTrue(j_avail)) {
        keb_log("OTA","firmware %s up-to-date", KEB_BK_FIRMWARE_VERSION);
        s_wait_s = OTA_CHECK_INTERVAL_S;
        cJSON_Delete(doc); return;
    }

    cJSON *j_ver = cJSON_GetObjectItem(doc,"version");
    cJSON *j_url = cJSON_GetObjectItem(doc,"url");
    cJSON *j_sha = cJSON_GetObjectItem(doc,"sha256");
    cJSON *j_sig = cJSON_GetObjectItem(doc,"ed25519_sig");

    if (!cJSON_IsString(j_ver)||!cJSON_IsString(j_url)||
        !cJSON_IsString(j_sha)||!cJSON_IsString(j_sig)) {
        keb_log("OTA","manifest onvolledig"); cJSON_Delete(doc); return;
    }

    if (!ota_is_newer(j_ver->valuestring, KEB_BK_FIRMWARE_VERSION)) {
        keb_log("OTA","firmware %s up-to-date (server: %s)",
                KEB_BK_FIRMWARE_VERSION, j_ver->valuestring);
        s_wait_s = OTA_CHECK_INTERVAL_S;
        cJSON_Delete(doc); return;
    }

    const char *sha_hex = j_sha->valuestring;
    if (strlen(sha_hex) != 64) {
        keb_log("OTA","sha256 ongeldige lengte"); cJSON_Delete(doc); return;
    }
    uint8_t sha_bytes[32];
    for (int i = 0; i < 32; i++) {
        unsigned bv = 0;
        if (sscanf(sha_hex+2*i,"%02x",&bv)!=1) {
            keb_log("OTA","sha256 hex decode fout"); cJSON_Delete(doc); return;
        }
        sha_bytes[i]=(uint8_t)bv;
    }

    // Pre-verify Ed25519 signature BEFORE downloading the binary
    if (!ota_verify(j_sig->valuestring, sha_bytes)) {
        keb_log("OTA","Ed25519 handtekening ongeldig — geweigerd");
        s_wait_s = OTA_RETRY_INTERVAL_S;
        cJSON_Delete(doc); return;
    }

    keb_log("OTA","update: %s → %s (sig OK)",
            KEB_BK_FIRMWARE_VERSION, j_ver->valuestring);

    strncpy(s_dl_url,    j_url->valuestring, sizeof(s_dl_url)-1);
    strncpy(s_sha256_hex, sha_hex,           sizeof(s_sha256_hex)-1);
    strncpy(s_sig_b64,  j_sig->valuestring,  sizeof(s_sig_b64)-1);
    memcpy(s_sha256_expected, sha_bytes, 32);
    s_download_pending = true;
    s_wait_s = OTA_CHECK_INTERVAL_S;

    cJSON_Delete(doc);
}

// ============================================================================
// Manifest callbacks (primary and secondary)
// ============================================================================

static void on_secondary(int status, const char *body, void *user);

// Pre-built check URLs (built once in OTA_Update before firing the request)
static char s_primary_url[192];
static char s_secondary_url[192];

static void on_primary(int status, const char *body, void *user) {
    (void)user;
    if (status != 200 || !body) {
        keb_log("OTA","primary HTTP %d — probeer secondary", status);
        keb_http_get(s_secondary_url, 10000, on_secondary, NULL);
        return;
    }
    on_manifest_ok(body);
    s_in_flight = false;
    s_tick_s    = 0;
}

static void on_secondary(int status, const char *body, void *user) {
    (void)user;
    s_in_flight = false;
    s_tick_s    = 0;
    if (status != 200 || !body) {
        keb_log("OTA","secondary HTTP %d — retry over %us", status, OTA_RETRY_INTERVAL_S);
        s_wait_s = OTA_RETRY_INTERVAL_S;
        return;
    }
    on_manifest_ok(body);
}

// ============================================================================
// Firmware download streaming callback (BK7231N only)
// ============================================================================

#if ENABLE_SEND_POSTANDGET && PLATFORM_BEKEN

static int ota_dl_cb(httprequest_t *req) {
    if (req->state == 0) {
        init_ota(START_ADR_OF_BK_PARTITION_OTA);
        sha256_init(&s_sha256_ctx);
        keb_log("OTA","download gestart");

    } else if (req->state == 1) {
        int len = (int)req->client_data.response_buf_filled;
        if (len > 0) {
            unsigned char *d = (unsigned char*)req->client_data.response_buf;
            add_otadata(d, len);
            sha256_update(&s_sha256_ctx, d, (size_t)len);
        }

    } else if (req->state == 2) {
        close_ota();
        uint8_t computed[32];
        sha256_final(&s_sha256_ctx, computed);

        char hex[65];
        for (int i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",computed[i]);
        hex[64]='\0';

        if (strcmp(hex, s_sha256_hex) != 0) {
            keb_log("OTA","SHA-256 MISMATCH — firmware geweigerd");
            keb_log("OTA","  verwacht: %s", s_sha256_hex);
            keb_log("OTA","  berekend: %s", hex);
            s_in_flight = false;
            s_tick_s    = 0;
            s_wait_s    = OTA_RETRY_INTERVAL_S;
            return 0;
        }

        keb_log("OTA","verificatie OK — herstarten");
        CFG_Save_IfThereArePendingChanges();
        bk_reboot();

    } else {
        keb_log("OTA","download fout (state=%d)", req->state);
        s_in_flight = false;
        s_tick_s    = 0;
        s_wait_s    = OTA_RETRY_INTERVAL_S;
    }
    return 0;
}

static void ota_start_download(void) {
    memset(&s_dl_req, 0, sizeof(s_dl_req));
    s_dl_req.url                          = s_dl_url;
    s_dl_req.method                       = HTTPCLIENT_GET;
    s_dl_req.port                         = 80;
    s_dl_req.timeout                      = 60000;
    s_dl_req.client_data.response_buf     = s_dl_buf;
    s_dl_req.client_data.response_buf_len = sizeof(s_dl_buf);
    s_dl_req.data_callback                = ota_dl_cb;
    HTTPClient_Async_SendGeneric(&s_dl_req);
    keb_log("OTA","download van: %s", s_dl_url);
}

#else

static void ota_start_download(void) {
    keb_log("OTA","streaming download niet beschikbaar op dit platform");
    s_in_flight      = false;
    s_download_pending = false;
    s_tick_s         = 0;
    s_wait_s         = OTA_CHECK_INTERVAL_S;
}

#endif // ENABLE_SEND_POSTANDGET && PLATFORM_BEKEN

// ============================================================================
// Public API
// ============================================================================

void OTA_Init(void) {
    keb_log("OTA","v%s — eerste check over %us", KEB_BK_FIRMWARE_VERSION, OTA_INITIAL_DELAY_S);
}

void OTA_Update(void) {
    if (s_in_flight) return;

    // Pending download (manifest verified, awaiting flash write)
    if (s_download_pending) {
        s_download_pending = false;
        s_in_flight        = true;
        ota_start_download();
        return;
    }

    // Timer-driven manifest check
    s_tick_s++;
    if (s_tick_s >= s_wait_s) {
        s_tick_s    = 0;
        s_in_flight = true;
        snprintf(s_primary_url,   sizeof(s_primary_url),
                 OTA_PRIMARY_HOST "/ota/check?version=%s&channel=" OTA_CHANNEL,
                 KEB_BK_FIRMWARE_VERSION);
        snprintf(s_secondary_url, sizeof(s_secondary_url),
                 OTA_SECONDARY_HOST "/ota/check?version=%s&channel=" OTA_CHANNEL,
                 KEB_BK_FIRMWARE_VERSION);
        keb_http_get(s_primary_url, 10000, on_primary, NULL);
    }
}
