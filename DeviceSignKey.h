#pragma once

#include <array>
#include <cstdint>
#include <string>

// DeviceSignKey
// - Stores a per-machine Ed25519 signing key in %ProgramData% protected with DPAPI (LocalMachine).
// - The private key is NOT stored in AppData.
// - Public key is sent to the site at device enrollment.
// - Signatures are used for /api/device_refresh.php request authentication.
//
// File path:
//   %ProgramData%\HayateKomorebi\DeviceKeys\device_ed25519_sign.key
//
// File format:
//   "HKED1" (5 bytes)
//   u32 enc_len_le
//   enc_sk_bytes (DPAPI machine protected, should decrypt to 64 bytes secret key)

namespace DeviceSignKey
{
    // Loads existing key or creates a new one.
    // Returns base64 of the 32-byte public key.
    bool GetOrCreatePublicKeyBase64(std::string& outPublicKeyB64, std::string* outError);

    // Signs a UTF-8 message and returns base64 of detached signature (64 bytes).
    bool SignDetachedBase64(const std::string& messageUtf8, std::string& outSignatureB64, std::string* outError);

    // Deletes the stored key file (recovery).
    bool DeleteStoredKey(std::string* outError);
}
