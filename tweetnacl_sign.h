// tweetnacl_sign.h
//
// Minimal subset of TweetNaCl (Ed25519 signatures + SHA-512) needed by HayateKomorebi.
//
// TweetNaCl is written and placed in the public domain by Daniel J. Bernstein.
// This file is a small, self-contained subset derived from the TweetNaCl API.
//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Random bytes provider (must be implemented by the embedding application).
void randombytes(uint8_t* out, uint64_t outlen);

// SHA-512
int crypto_hash(uint8_t* out /* 64 bytes */, const uint8_t* m, uint64_t n);

// Ed25519 signatures (compatible with libsodium's crypto_sign_*).
#define crypto_sign_PUBLICKEYBYTES 32
#define crypto_sign_SECRETKEYBYTES 64
#define crypto_sign_BYTES 64

int crypto_sign_keypair(uint8_t* pk /* 32 bytes */, uint8_t* sk /* 64 bytes */);
int crypto_sign(uint8_t* sm, uint64_t* smlen, const uint8_t* m, uint64_t n, const uint8_t* sk);
int crypto_sign_open(uint8_t* m, uint64_t* mlen, const uint8_t* sm, uint64_t n, const uint8_t* pk);

#ifdef __cplusplus
}
#endif
