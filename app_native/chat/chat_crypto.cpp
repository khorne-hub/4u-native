// The chat's cryptography: chat_crypto.h. SHA-256, HMAC, HKDF, ChaCha20 and base 64 are written here (small and
// well known); the elliptic curve is libsecp256k1's (third_party/secp256k1, MIT; built by chat_secp256k1.c).
#define SECP256K1_NO_API_VISIBILITY_ATTRIBUTES 1
#include "chat_crypto.h"

#include <cstring>
#include <mutex>

#include "secp256k1.h"
#include "secp256k1_ecdh.h"
#include "secp256k1_extrakeys.h"
#include "secp256k1_schnorrsig.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <stdlib.h>
#endif

namespace chatc {

void random_bytes(void *out, size_t n) {
#ifdef _WIN32
    BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    arc4random_buf(out, n);
#endif
}

// ---- SHA-256 (FIPS 180-4)
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}
Sha256::Sha256() : h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, len(0), fill(0) {}
void Sha256::add(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    len += n;
    while (n) {
        size_t k = std::min(n, 64 - fill);
        memcpy(buf + fill, p, k);
        fill += k, p += k, n -= k;
        if (fill == 64) block(h, buf), fill = 0;
    }
}
void Sha256::done(uint8_t out[32]) {
    uint64_t bits = len * 8;
    uint8_t pad = 0x80, zero = 0;
    add(&pad, 1);
    while (fill != 56) add(&zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    add(l, 8);
    for (int i = 0; i < 8; i++) out[4 * i] = h[i] >> 24, out[4 * i + 1] = h[i] >> 16, out[4 * i + 2] = h[i] >> 8, out[4 * i + 3] = h[i];
}
void sha256(const void *p, size_t n, uint8_t out[32]) {
    Sha256 s;
    s.add(p, n);
    s.done(out);
}
void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[32];
    if (klen > 64) sha256(key, klen, k);
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) ipad[i] = k[i] ^ 0x36, opad[i] = k[i] ^ 0x5c;
    Sha256 a;
    a.add(ipad, 64), a.add(msg, mlen), a.done(inner);
    Sha256 b;
    b.add(opad, 64), b.add(inner, 32), b.done(out);
}

// ---- text forms
std::string hex(const uint8_t *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; i++) s[2 * i] = d[p[i] >> 4], s[2 * i + 1] = d[p[i] & 15];
    return s;
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool unhex(const std::string &s, uint8_t *out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(const uint8_t *p, size_t n) {
    std::string s;
    s.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        s += B64[v >> 18 & 63], s += B64[v >> 12 & 63];
        s += i + 1 < n ? B64[v >> 6 & 63] : '=';
        s += i + 2 < n ? B64[v & 63] : '=';
    }
    return s;
}
bool unbase64(const std::string &s, std::string &out) {
    out.clear();
    if (s.size() % 4) return false;
    uint32_t v = 0;
    int bits = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '=') {
            if (i + 2 < s.size() || (i + 1 < s.size() && s[i + 1] != '=')) return false;
            break;
        }
        const char *at = strchr(B64, c);
        if (!c || !at) return false;
        v = v << 6 | (uint32_t)(at - B64), bits += 6;
        if (bits >= 8) bits -= 8, out += (char)(v >> bits & 255);
    }
    return true;
}

// ---- secp256k1
static secp256k1_context *ctx() {
    static secp256k1_context *c;
    static std::once_flag once;
    std::call_once(once, [] {
        c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        uint8_t seed[32];
        random_bytes(seed, 32);
        if (!secp256k1_context_randomize(c, seed)) {}
    });
    return c;
}
void new_secret(uint8_t sec[32]) {
    do random_bytes(sec, 32);
    while (!secp256k1_ec_seckey_verify(ctx(), sec));
}
bool public_key(const uint8_t sec[32], uint8_t pub[32]) {
    secp256k1_keypair kp;
    secp256k1_xonly_pubkey x;
    return secp256k1_keypair_create(ctx(), &kp, sec) && secp256k1_keypair_xonly_pub(ctx(), &x, nullptr, &kp) &&
           secp256k1_xonly_pubkey_serialize(ctx(), pub, &x);
}
bool sign(const uint8_t sec[32], const uint8_t msg[32], uint8_t sig[64]) {
    secp256k1_keypair kp;
    uint8_t aux[32];
    random_bytes(aux, 32);
    return secp256k1_keypair_create(ctx(), &kp, sec) && secp256k1_schnorrsig_sign32(ctx(), sig, msg, &kp, aux);
}
bool verify(const uint8_t pub[32], const uint8_t msg[32], const uint8_t sig[64]) {
    secp256k1_xonly_pubkey x;
    return secp256k1_xonly_pubkey_parse(ctx(), &x, pub) && secp256k1_schnorrsig_verify(ctx(), sig, msg, 32, &x);
}

// ---- NIP-44 v2
static int copy_x(unsigned char *out, const unsigned char *x, const unsigned char *, void *) {
    memcpy(out, x, 32);
    return 1;
}
bool conversation_key(const uint8_t sec[32], const uint8_t their_pub[32], uint8_t key[32]) {
    uint8_t c[33], shared[32];
    c[0] = 2;
    memcpy(c + 1, their_pub, 32);
    secp256k1_pubkey pk;
    if (!secp256k1_ec_pubkey_parse(ctx(), &pk, c, 33) || !secp256k1_ecdh(ctx(), shared, &pk, sec, copy_x, nullptr)) return false;
    hmac_sha256((const uint8_t *)"nip44-v2", 8, shared, 32, key);  // (HKDF-extract: the salt is the key)
    return true;
}
static void message_keys(const uint8_t ck[32], const uint8_t nonce[32], uint8_t out[76]) {  // HKDF-expand, 76 bytes
    uint8_t t[32], in[32 + 32 + 1];
    size_t got = 0, tl = 0;
    for (uint8_t i = 1; got < 76; i++) {
        memcpy(in, t, tl);
        memcpy(in + tl, nonce, 32);
        in[tl + 32] = i;
        hmac_sha256(ck, 32, in, tl + 33, t);
        tl = 32;
        size_t k = std::min<size_t>(32, 76 - got);
        memcpy(out + got, t, k);
        got += k;
    }
}
static inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static void chacha20(const uint8_t key[32], const uint8_t nonce[12], uint8_t *data, size_t n) {  // RFC 8439, counter 0
    uint32_t st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) memcpy(&st[4 + i], key + 4 * i, 4);
    st[12] = 0;
    for (int i = 0; i < 3; i++) memcpy(&st[13 + i], nonce + 4 * i, 4);
    for (size_t off = 0; off < n; off += 64) {
        uint32_t x[16];
        memcpy(x, st, sizeof x);
        auto qr = [&x](int a, int b, int c, int d) {
            x[a] += x[b], x[d] = rol(x[d] ^ x[a], 16);
            x[c] += x[d], x[b] = rol(x[b] ^ x[c], 12);
            x[a] += x[b], x[d] = rol(x[d] ^ x[a], 8);
            x[c] += x[d], x[b] = rol(x[b] ^ x[c], 7);
        };
        for (int r = 0; r < 10; r++) {
            qr(0, 4, 8, 12), qr(1, 5, 9, 13), qr(2, 6, 10, 14), qr(3, 7, 11, 15);
            qr(0, 5, 10, 15), qr(1, 6, 11, 12), qr(2, 7, 8, 13), qr(3, 4, 9, 14);
        }
        uint8_t ks[64];
        for (int i = 0; i < 16; i++) {
            uint32_t v = x[i] + st[i];
            ks[4 * i] = v, ks[4 * i + 1] = v >> 8, ks[4 * i + 2] = v >> 16, ks[4 * i + 3] = v >> 24;  // (little endian)
        }
        for (size_t i = 0; i < 64 && off + i < n; i++) data[off + i] ^= ks[i];
        st[12]++;
    }
}
static size_t padded_len(size_t n) {
    if (n <= 32) return 32;
    size_t next = 1;
    while (next < n) next <<= 1;  // (1 << (floor(log2(n - 1)) + 1))
    size_t chunk = next <= 256 ? 32 : next / 8;
    return chunk * ((n - 1) / chunk + 1);
}
bool nip44_encrypt(const uint8_t ck[32], const std::string &plain, std::string &payload, const uint8_t *nonce_in) {
    if (plain.empty() || plain.size() > 65535) return false;
    uint8_t nonce[32], keys[76];
    if (nonce_in) memcpy(nonce, nonce_in, 32);
    else random_bytes(nonce, 32);
    message_keys(ck, nonce, keys);
    std::string body(2 + padded_len(plain.size()), '\0');
    body[0] = (char)(plain.size() >> 8), body[1] = (char)(plain.size() & 255);
    memcpy(&body[2], plain.data(), plain.size());
    chacha20(keys, keys + 32, (uint8_t *)&body[0], body.size());
    std::string mac_in((const char *)nonce, 32);
    mac_in += body;
    uint8_t mac[32];
    hmac_sha256(keys + 44, 32, (const uint8_t *)mac_in.data(), mac_in.size(), mac);
    std::string all(1, '\x02');
    all.append((const char *)nonce, 32);
    all += body;
    all.append((const char *)mac, 32);
    payload = base64((const uint8_t *)all.data(), all.size());
    return true;
}
bool nip44_decrypt(const uint8_t ck[32], const std::string &payload, std::string &plain) {
    if (payload.size() < 132 || payload.size() > 87472 || payload[0] == '#') return false;
    std::string raw;
    if (!unbase64(payload, raw) || raw.size() < 99 || raw.size() > 65603 || raw[0] != 2) return false;
    const uint8_t *nonce = (const uint8_t *)raw.data() + 1;
    uint8_t keys[76], mac[32];
    message_keys(ck, nonce, keys);
    hmac_sha256(keys + 44, 32, (const uint8_t *)raw.data() + 1, raw.size() - 33, mac);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= mac[i] ^ (uint8_t)raw[raw.size() - 32 + i];
    if (diff) return false;
    std::string body = raw.substr(33, raw.size() - 65);
    chacha20(keys, keys + 32, (uint8_t *)&body[0], body.size());
    size_t n = (uint8_t)body[0] << 8 | (uint8_t)body[1];
    if (!n || 2 + n > body.size() || body.size() != 2 + padded_len(n)) return false;
    plain = body.substr(2, n);
    return true;
}

// ---- friend codes
static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
std::string friend_code(const uint8_t pub[32]) {
    Sha256 s;
    s.add("mh-chat-code:", 13);
    s.add(pub, 32);
    uint8_t h[32];
    s.done(h);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | h[i];
    std::string code;
    for (int i = 0; i < 12; i++) code += CROCKFORD[(v >> (59 - 5 * i)) & 31];
    return code;
}
std::string normalize_code(const std::string &typed) {
    std::string c;
    for (char ch : typed) {
        if (ch == '-' || ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (ch == 'I' || ch == 'L') ch = '1';
        if (ch == 'O') ch = '0';
        if (!strchr(CROCKFORD, ch) || !ch) return "";
        c += ch;
    }
    return c.size() == 12 ? c : "";
}
std::string show_code(const std::string &code) {
    if (code.size() != 12) return code;
    return code.substr(0, 4) + "-" + code.substr(4, 4) + "-" + code.substr(8, 4);
}

}  // namespace chatc
