#pragma once

#include <string>

namespace DeviceKeyCrypto {

// Returns server public key string for site DB.
// Format: "HKP1:<base64(BCRYPT_ECCPUBLIC_BLOB)>"
// Source key: C:\ProgramData\HayateKomorebi\DeviceKeys\server_ecdh_p256.key (HKDK format)
bool GetServerPublicKeyForDb(std::string& outPublicKeyPem);

} // namespace DeviceKeyCrypto
