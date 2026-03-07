#pragma once
// hk_tweetnacl_sign.h
//
// Minimal Ed25519 signing API compatible with TweetNaCl/libsodium.
//
// Public domain algorithms (TweetNaCl lineage).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Random bytes provider (must be implemented by the embedding application).
void randombytes(uint8_t* out, uint64_t outlen);

#define crypto_sign_PUBLICKEYBYTES 32
#define crypto_sign_SECRETKEYBYTES 64
#define crypto_sign_BYTES 64

// Generates Ed25519 keypair.
// - pk: 32 bytes
// - sk: 64 bytes (seed[32] || pk[32])
int crypto_sign_keypair(uint8_t* pk, uint8_t* sk);

// Signs message m and outputs signed message sm = sig(64) || m.
// - sm must have space for n + 64 bytes.
int crypto_sign(uint8_t* sm, uint64_t* smlen, const uint8_t* m, uint64_t n, const uint8_t* sk);

#ifdef __cplusplus
}
#endif
