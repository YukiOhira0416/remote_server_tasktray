#pragma once

#include <array>
#include <cstdint>
#include <string>

// Stores an Ed25519 device signing key in ProgramData protected by DPAPI (LocalMachine).
// The secret key is NOT stored in AppData.
class Ed25519DeviceKey
{
public:
    // Gets existing key or creates a new one.
    // Returns base64 of the 32-byte public key.
    static bool GetOrCreatePublicKeyBase64(std::string& outPublicKeyB64, std::string* outError);

    // Signs a message and returns base64 of detached signature (64 bytes).
    static bool SignDetachedBase64(const std::string& messageUtf8, std::string& outSignatureB64, std::string* outError);

    // Deletes the stored device key file.
    // Intended for recovery flows where the dashboard forces re-pairing.
    static bool DeleteStoredKey(std::string* outError);

private:
    static bool LoadOrCreateSecretKey(std::array<uint8_t, 64>& outSk, std::string* outError);
    static bool EnsureProgramDataDirectory(std::wstring& outDirPath, std::string* outError);
    static std::wstring GetKeyFilePath();

    static bool Base64Encode(const uint8_t* data, size_t dataLen, std::string& outB64, std::string* outError);
    static void SetError(std::string* outError, const char* msg);
};
