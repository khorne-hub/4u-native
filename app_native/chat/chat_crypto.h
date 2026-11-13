// The chat's cryptography (runtime/chat/chat.h): the player's identity is a secp256k1 key made on the device (libsecp256k1,
// third_party/secp256k1, MIT), as a Nostr key: its public half (x only, 32 bytes) names the player everywhere, and every
// message the player sends is signed with it (BIP-340 Schnorr). Messages between two friends are encrypted as Nostr's
// NIP-44 v2 says (ECDH, HKDF-SHA256, ChaCha20, HMAC-SHA256), so only the two of them can read them.
// The same file is in both ports (MHGU: runtime/chat, MH4U for Android: app_native/chat).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace chatc {

void random_bytes(void *out, size_t n);  // the system's (arc4random_buf, BCryptGenRandom)

struct Sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
    Sha256();
    void add(const void *p, size_t n);
    void done(uint8_t out[32]);
};
void sha256(const void *p, size_t n, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]);

std::string hex(const uint8_t *p, size_t n);
bool unhex(const std::string &s, uint8_t *out, size_t n);  // exactly 2n hex digits
std::string base64(const uint8_t *p, size_t n);
bool unbase64(const std::string &s, std::string &out);

// A new secret key (32 bytes, valid for secp256k1); its public key, x only
void new_secret(uint8_t sec[32]);
bool public_key(const uint8_t sec[32], uint8_t pub[32]);
// BIP-340 over a 32-byte message (a Nostr event's id)
bool sign(const uint8_t sec[32], const uint8_t msg[32], uint8_t sig[64]);
bool verify(const uint8_t pub[32], const uint8_t msg[32], const uint8_t sig[64]);

// NIP-44 v2: the key two players share (from either side), then a message sealed and opened with it
bool conversation_key(const uint8_t sec[32], const uint8_t their_pub[32], uint8_t key[32]);
bool nip44_encrypt(const uint8_t key[32], const std::string &plain, std::string &payload, const uint8_t *nonce = nullptr);
bool nip44_decrypt(const uint8_t key[32], const std::string &payload, std::string &plain);

// The friend code of a public key: 12 letters and digits (Crockford's base 32: no I, L, O or U) from a hash of it, shown
// as XXXX-XXXX-XXXX. 60 bits: finding another key with the same code would take some 10^18 tries.
std::string friend_code(const uint8_t pub[32]);
// A code as someone typed it: the 12 characters, upper case, I/L read as 1 and O as 0; "" if it is not one
std::string normalize_code(const std::string &typed);
std::string show_code(const std::string &code);  // XXXX-XXXX-XXXX

}  // namespace chatc
