#include "SecureLineCrypto.h"

#include "DebugLog.h"

#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace hk_secureline
{
    static std::wstring GetProgramDataPath()
    {
        PWSTR path = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &path);
        if (FAILED(hr) || !path) {
            return L"C:\\ProgramData";
        }
        std::wstring ret(path);
        CoTaskMemFree(path);
        return ret;
    }

    static bool EnsureDirExists(const std::wstring& dir)
    {
        if (dir.empty()) {
            return false;
        }
        DWORD attr = GetFileAttributesW(dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return true;
        }

        // Create parent first.
        std::wstring parent = dir;
        while (!parent.empty() && (parent.back() == L'\\' || parent.back() == L'/')) {
            parent.pop_back();
        }
        size_t pos = parent.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring parentDir = parent.substr(0, pos);
            if (!parentDir.empty()) {
                EnsureDirExists(parentDir);
            }
        }

        if (CreateDirectoryW(dir.c_str(), nullptr) != 0) {
            return true;
        }
        DWORD err = GetLastError();
        return (err == ERROR_ALREADY_EXISTS);
    }

    static bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>& out)
    {
        out.clear();
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            return false;
        }
        ifs.seekg(0, std::ios::end);
        std::streamoff len = ifs.tellg();
        if (len <= 0) {
            return false;
        }
        ifs.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(len));
        if (!ifs.read(reinterpret_cast<char*>(out.data()), len)) {
            out.clear();
            return false;
        }
        return true;
    }

    static bool WriteFileAll(const std::wstring& path, const std::vector<uint8_t>& data)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return false;
        }
        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(ofs);
    }

    static bool Base64Encode(const std::vector<uint8_t>& bin, std::string& out)
    {
        out.clear();
        if (bin.empty()) {
            out = "";
            return true;
        }
        DWORD needed = 0;
        if (!CryptBinaryToStringA(bin.data(), static_cast<DWORD>(bin.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed)) {
            return false;
        }
        std::string buf;
        buf.resize(needed);
        if (!CryptBinaryToStringA(bin.data(), static_cast<DWORD>(bin.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &buf[0], &needed)) {
            return false;
        }
        // needed includes '\0'
        if (!buf.empty() && buf.back() == '\0') {
            buf.pop_back();
        }
        out.swap(buf);
        return true;
    }

    static bool Base64Decode(const std::string& b64, std::vector<uint8_t>& out)
    {
        out.clear();
        if (b64.empty()) {
            return true;
        }
        DWORD needed = 0;
        if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &needed, nullptr, nullptr)) {
            return false;
        }
        out.resize(needed);
        if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, out.data(), &needed, nullptr, nullptr)) {
            out.clear();
            return false;
        }
        out.resize(needed);
        return true;
    }

    static bool DpapiProtectMachine(const std::vector<uint8_t>& plain, std::vector<uint8_t>& out)
    {
        out.clear();
        DATA_BLOB in{};
        in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plain.data()));
        in.cbData = static_cast<DWORD>(plain.size());

        DATA_BLOB outBlob{};
        if (!CryptProtectData(&in, L"hk_secureline", nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE, &outBlob)) {
            return false;
        }

        out.assign(outBlob.pbData, outBlob.pbData + outBlob.cbData);
        LocalFree(outBlob.pbData);
        return true;
    }

    static bool DpapiUnprotectMachine(const std::vector<uint8_t>& enc, std::vector<uint8_t>& out)
    {
        out.clear();
        DATA_BLOB in{};
        in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(enc.data()));
        in.cbData = static_cast<DWORD>(enc.size());

        DATA_BLOB outBlob{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &outBlob)) {
            return false;
        }

        out.assign(outBlob.pbData, outBlob.pbData + outBlob.cbData);
        LocalFree(outBlob.pbData);
        return true;
    }

    static std::wstring ServerKeyFilePath()
    {
        // NOTE:
        //   SecureLineCrypto uses its own persistent ECDH key file.
        //   The RemoteServer/RemoteService control & video encryption stack uses
        //   ProgramData\\HayateKomorebi\\DeviceKeys\\server_ecdh_p256.key with a *different* format ("HKDK").
        //   Using the same filename here caused a format collision and broke control/video crypto.
        //   Therefore this module must NOT use server_ecdh_p256.key.
        std::wstring base = GetProgramDataPath();
        std::wstring dir = base + L"\\HayateKomorebi\\DeviceKeys";
        EnsureDirExists(dir);
        return dir + L"\\server_line_ecdh_p256.key";
    }

    struct KeyPair
    {
        std::vector<uint8_t> pubBlob;
        std::vector<uint8_t> privBlob; // ECCPRIVATE_BLOB (decrypted)
    };

    static bool LoadOrCreateServerKeyPair(KeyPair& out)
    {
        out.pubBlob.clear();
        out.privBlob.clear();

        const std::wstring path = ServerKeyFilePath();
        std::vector<uint8_t> file;
        if (ReadFileAll(path, file)) {
            // file format: "HKDK1" + u32 pubLen + u32 encPrivLen + pub + encPriv
            if (file.size() > 16 && std::memcmp(file.data(), "HKDK1", 5) == 0) {
                const uint8_t* p = file.data() + 5;
                const uint8_t* end = file.data() + file.size();
                auto read_u32 = [&](uint32_t& v) -> bool {
                    if (p + 4 > end) return false;
                    std::memcpy(&v, p, 4);
                    p += 4;
                    return true;
                };
                uint32_t pubLen = 0, encPrivLen = 0;
                if (read_u32(pubLen) && read_u32(encPrivLen)) {
                    if (p + pubLen + encPrivLen <= end) {
                        out.pubBlob.assign(p, p + pubLen);
                        p += pubLen;
                        std::vector<uint8_t> encPriv(p, p + encPrivLen);
                        std::vector<uint8_t> priv;
                        if (DpapiUnprotectMachine(encPriv, priv)) {
                            out.privBlob.swap(priv);
                            if (!out.pubBlob.empty() && !out.privBlob.empty()) {
                                return true;
                            }
                        }
                    }
                }
            }
        }

        // Create new keypair.
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) != 0) {
            DebugLog("SecureLineCrypto: BCryptOpenAlgorithmProvider(ECDH_P256) failed.");
            return false;
        }

        BCRYPT_KEY_HANDLE key = nullptr;
        if (BCryptGenerateKeyPair(alg, &key, 256, 0) != 0) {
            DebugLog("SecureLineCrypto: BCryptGenerateKeyPair failed.");
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        if (BCryptFinalizeKeyPair(key, 0) != 0) {
            DebugLog("SecureLineCrypto: BCryptFinalizeKeyPair failed.");
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        auto export_blob = [&](LPCWSTR blobType, std::vector<uint8_t>& dst) -> bool {
            DWORD cb = 0;
            if (BCryptExportKey(key, nullptr, blobType, nullptr, 0, &cb, 0) != 0) {
                return false;
            }
            dst.resize(cb);
            if (BCryptExportKey(key, nullptr, blobType, dst.data(), cb, &cb, 0) != 0) {
                dst.clear();
                return false;
            }
            dst.resize(cb);
            return true;
        };

        if (!export_blob(BCRYPT_ECCPUBLIC_BLOB, out.pubBlob) || !export_blob(BCRYPT_ECCPRIVATE_BLOB, out.privBlob)) {
            DebugLog("SecureLineCrypto: BCryptExportKey failed.");
            BCryptDestroyKey(key);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);

        std::vector<uint8_t> encPriv;
        if (!DpapiProtectMachine(out.privBlob, encPriv)) {
            DebugLog("SecureLineCrypto: DPAPI protect failed.");
            return false;
        }

        std::vector<uint8_t> outFile;
        outFile.insert(outFile.end(), {'H','K','D','K','1'});
        auto append_u32 = [&](uint32_t v) {
            uint8_t b[4];
            std::memcpy(b, &v, 4);
            outFile.insert(outFile.end(), b, b + 4);
        };
        append_u32(static_cast<uint32_t>(out.pubBlob.size()));
        append_u32(static_cast<uint32_t>(encPriv.size()));
        outFile.insert(outFile.end(), out.pubBlob.begin(), out.pubBlob.end());
        outFile.insert(outFile.end(), encPriv.begin(), encPriv.end());

        if (!WriteFileAll(path, outFile)) {
            DebugLog("SecureLineCrypto: failed to write server key file.");
            // Still allow in-memory usage.
        }

        return true;
    }

    static bool Sha256(const std::vector<uint8_t>& data, std::array<uint8_t, 32>& out)
    {
        out.fill(0);
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            return false;
        }
        DWORD hashLen = 0;
        DWORD cb = 0;
        if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cb, 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        std::vector<uint8_t> hash(hashLen);
        BCRYPT_HASH_HANDLE h = nullptr;
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        if (!data.empty()) {
            if (BCryptHashData(h, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) != 0) {
                BCryptDestroyHash(h);
                BCryptCloseAlgorithmProvider(alg, 0);
                return false;
            }
        }
        if (BCryptFinishHash(h, hash.data(), static_cast<ULONG>(hash.size()), 0) != 0) {
            BCryptDestroyHash(h);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        BCryptDestroyHash(h);
        BCryptCloseAlgorithmProvider(alg, 0);

        // Truncate/fit to 32
        if (hash.size() < 32) {
            return false;
        }
        std::copy(hash.begin(), hash.begin() + 32, out.begin());
        return true;
    }

    static bool EcdhRawSecret(const std::vector<uint8_t>& myPrivBlob, const std::vector<uint8_t>& peerPubBlob, std::vector<uint8_t>& rawSecret)
    {
        rawSecret.clear();

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) != 0) {
            return false;
        }

        BCRYPT_KEY_HANDLE myKey = nullptr;
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &myKey,
                                const_cast<PUCHAR>(myPrivBlob.data()), static_cast<ULONG>(myPrivBlob.size()), 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        BCRYPT_KEY_HANDLE peerKey = nullptr;
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey,
                                const_cast<PUCHAR>(peerPubBlob.data()), static_cast<ULONG>(peerPubBlob.size()), 0) != 0) {
            BCryptDestroyKey(myKey);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        BCRYPT_SECRET_HANDLE secret = nullptr;
        if (BCryptSecretAgreement(myKey, peerKey, &secret, 0) != 0) {
            BCryptDestroyKey(peerKey);
            BCryptDestroyKey(myKey);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        DWORD cb = 0;
        if (BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, nullptr, 0, &cb, 0) != 0) {
            BCryptDestroySecret(secret);
            BCryptDestroyKey(peerKey);
            BCryptDestroyKey(myKey);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        rawSecret.resize(cb);
        if (BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, rawSecret.data(), cb, &cb, 0) != 0) {
            rawSecret.clear();
            BCryptDestroySecret(secret);
            BCryptDestroyKey(peerKey);
            BCryptDestroyKey(myKey);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        rawSecret.resize(cb);

        BCryptDestroySecret(secret);
        BCryptDestroyKey(peerKey);
        BCryptDestroyKey(myKey);
        BCryptCloseAlgorithmProvider(alg, 0);
        return true;
    }

    static bool RandomBytes(uint8_t* dst, size_t n)
    {
        if (!dst || n == 0) return false;
        if (BCryptGenRandom(nullptr, dst, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return false;
        }
        return true;
    }

    static bool AesGcmEncrypt(const std::array<uint8_t, 32>& key,
                             const uint8_t nonce[12],
                             const std::vector<uint8_t>& plain,
                             std::vector<uint8_t>& cipher,
                             uint8_t tag[16])
    {
        cipher.clear();
        std::memset(tag, 0, 16);

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
            return false;
        }

        // Set GCM mode.
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        DWORD objLen = 0, cb = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        std::vector<uint8_t> obj(objLen);

        BCRYPT_KEY_HANDLE hKey = nullptr;
        if (BCryptGenerateSymmetricKey(alg, &hKey, obj.data(), objLen,
                                       const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = 12;
        info.pbTag = tag;
        info.cbTag = 16;

        ULONG outLen = 0;
        // Query length
        if (BCryptEncrypt(hKey,
                          plain.empty() ? nullptr : const_cast<PUCHAR>(plain.data()),
                          static_cast<ULONG>(plain.size()),
                          &info,
                          nullptr, 0,
                          nullptr, 0,
                          &outLen,
                          0) != 0) {
            BCryptDestroyKey(hKey);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        cipher.resize(outLen);
        if (BCryptEncrypt(hKey,
                          plain.empty() ? nullptr : const_cast<PUCHAR>(plain.data()),
                          static_cast<ULONG>(plain.size()),
                          &info,
                          nullptr, 0,
                          cipher.data(),
                          outLen,
                          &outLen,
                          0) != 0) {
            cipher.clear();
            BCryptDestroyKey(hKey);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        cipher.resize(outLen);

        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(alg, 0);
        return true;
    }

    static bool AesGcmDecrypt(const std::array<uint8_t, 32>& key,
                             const uint8_t nonce[12],
                             const std::vector<uint8_t>& cipher,
                             const uint8_t tag[16],
                             std::vector<uint8_t>& plain)
    {
        plain.clear();

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
            return false;
        }
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        DWORD objLen = 0, cb = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        std::vector<uint8_t> obj(objLen);

        BCRYPT_KEY_HANDLE hKey = nullptr;
        if (BCryptGenerateSymmetricKey(alg, &hKey, obj.data(), objLen,
                                       const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) != 0) {
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        uint8_t tagCopy[16];
        std::memcpy(tagCopy, tag, 16);

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = 12;
        info.pbTag = tagCopy;
        info.cbTag = 16;

        ULONG outLen = 0;
        if (BCryptDecrypt(hKey,
                          cipher.empty() ? nullptr : const_cast<PUCHAR>(cipher.data()),
                          static_cast<ULONG>(cipher.size()),
                          &info,
                          nullptr, 0,
                          nullptr, 0,
                          &outLen,
                          0) != 0) {
            BCryptDestroyKey(hKey);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }

        plain.resize(outLen);
        if (BCryptDecrypt(hKey,
                          cipher.empty() ? nullptr : const_cast<PUCHAR>(cipher.data()),
                          static_cast<ULONG>(cipher.size()),
                          &info,
                          nullptr, 0,
                          plain.data(),
                          outLen,
                          &outLen,
                          0) != 0) {
            plain.clear();
            BCryptDestroyKey(hKey);
            BCryptCloseAlgorithmProvider(alg, 0);
            return false;
        }
        plain.resize(outLen);

        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(alg, 0);
        return true;
    }

    static bool RecvUntilNewline(SOCKET sock, std::string& recvBuffer, std::string& outLine, int timeoutMs)
    {
        outLine.clear();

        // First try from existing buffer
        for (;;) {
            size_t pos = recvBuffer.find('\n');
            if (pos != std::string::npos) {
                outLine = recvBuffer.substr(0, pos);
                recvBuffer.erase(0, pos + 1);
                if (!outLine.empty() && outLine.back() == '\r') {
                    outLine.pop_back();
                }
                return true;
            }

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);

            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            int sel = select(0, &readSet, nullptr, nullptr, &tv);
            if (sel == SOCKET_ERROR) {
                return false;
            }
            if (sel == 0) {
                // timeout
                return false;
            }

            char buf[512];
            int received = recv(sock, buf, sizeof(buf), 0);
            if (received <= 0) {
                return false;
            }
            recvBuffer.append(buf, received);
        }
    }

    static bool SendAll(SOCKET sock, const char* data, int len)
    {
        int sentTotal = 0;
        while (sentTotal < len) {
            int sent = send(sock, data + sentTotal, len - sentTotal, 0);
            if (sent == SOCKET_ERROR || sent == 0) {
                return false;
            }
            sentTotal += sent;
        }
        return true;
    }

    bool ServerHandshake(SOCKET sock, std::string& recvBuffer, Session& outSession, int timeoutMs)
    {
        Clear(outSession);

        KeyPair kp;
        if (!LoadOrCreateServerKeyPair(kp)) {
            DebugLog("SecureLineCrypto: ServerHandshake: failed to load/create server key pair.");
            return false;
        }

        std::string line;
        if (!RecvUntilNewline(sock, recvBuffer, line, timeoutMs)) {
            DebugLog("SecureLineCrypto: ServerHandshake: failed to receive HELLO1.");
            return false;
        }

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd != "HELLO1") {
            DebugLog("SecureLineCrypto: ServerHandshake: unexpected first line: " + line);
            return false;
        }

        std::string b64PeerPub, b64ClientNonce;
        iss >> b64PeerPub >> b64ClientNonce;
        if (b64PeerPub.empty() || b64ClientNonce.empty()) {
            DebugLog("SecureLineCrypto: ServerHandshake: invalid HELLO1 format.");
            return false;
        }

        std::vector<uint8_t> peerPub;
        std::vector<uint8_t> clientNonce;
        if (!Base64Decode(b64PeerPub, peerPub) || !Base64Decode(b64ClientNonce, clientNonce) || clientNonce.size() != 12) {
            DebugLog("SecureLineCrypto: ServerHandshake: base64 decode failed.");
            return false;
        }

        std::vector<uint8_t> rawSecret;
        if (!EcdhRawSecret(kp.privBlob, peerPub, rawSecret)) {
            DebugLog("SecureLineCrypto: ServerHandshake: ECDH raw secret failed.");
            return false;
        }

        uint8_t serverNonce[12];
        if (!RandomBytes(serverNonce, sizeof(serverNonce))) {
            DebugLog("SecureLineCrypto: ServerHandshake: RNG failed.");
            return false;
        }

        // Derive session key
        std::vector<uint8_t> kdf;
        const char label[] = "HK-SESS1";
        kdf.insert(kdf.end(), label, label + sizeof(label) - 1);
        kdf.insert(kdf.end(), rawSecret.begin(), rawSecret.end());
        kdf.insert(kdf.end(), clientNonce.begin(), clientNonce.end());
        kdf.insert(kdf.end(), serverNonce, serverNonce + 12);

        if (!Sha256(kdf, outSession.key)) {
            DebugLog("SecureLineCrypto: ServerHandshake: SHA256 failed.");
            return false;
        }

        outSession.active = true;

        // Send WELCOME1
        std::string b64ServerPub, b64ServerNonce;
        if (!Base64Encode(kp.pubBlob, b64ServerPub)) {
            Clear(outSession);
            return false;
        }
        std::vector<uint8_t> serverNonceVec(serverNonce, serverNonce + 12);
        if (!Base64Encode(serverNonceVec, b64ServerNonce)) {
            Clear(outSession);
            return false;
        }

        std::string welcome = "WELCOME1 " + b64ServerPub + " " + b64ServerNonce + "\n";
        if (!SendAll(sock, welcome.c_str(), static_cast<int>(welcome.size()))) {
            DebugLog("SecureLineCrypto: ServerHandshake: send WELCOME1 failed.");
            Clear(outSession);
            return false;
        }

        return true;
    }

    bool EncryptLineToSec1(const Session& session, const std::string& plainLine, std::string& outSecLine)
    {
        outSecLine.clear();
        if (!session.active) {
            return false;
        }

        std::vector<uint8_t> plain(plainLine.begin(), plainLine.end());
        uint8_t nonce[12];
        if (!RandomBytes(nonce, sizeof(nonce))) {
            return false;
        }
        uint8_t tag[16];
        std::vector<uint8_t> cipher;
        if (!AesGcmEncrypt(session.key, nonce, plain, cipher, tag)) {
            return false;
        }

        std::vector<uint8_t> record;
        record.insert(record.end(), nonce, nonce + 12);
        record.insert(record.end(), tag, tag + 16);
        record.insert(record.end(), cipher.begin(), cipher.end());

        std::string b64;
        if (!Base64Encode(record, b64)) {
            return false;
        }

        outSecLine = "SEC1 " + b64 + "\n";
        return true;
    }

    bool DecryptSec1ToLine(const Session& session, const std::string& secLine, std::string& outPlainLine)
    {
        outPlainLine.clear();
        if (!session.active) {
            return false;
        }

        std::istringstream iss(secLine);
        std::string prefix;
        iss >> prefix;
        if (prefix != "SEC1") {
            return false;
        }
        std::string b64;
        iss >> b64;
        if (b64.empty()) {
            return false;
        }

        std::vector<uint8_t> record;
        if (!Base64Decode(b64, record)) {
            return false;
        }
        if (record.size() < 12 + 16) {
            return false;
        }
        const uint8_t* nonce = record.data();
        const uint8_t* tag = record.data() + 12;
        std::vector<uint8_t> cipher(record.begin() + 12 + 16, record.end());

        std::vector<uint8_t> plain;
        if (!AesGcmDecrypt(session.key, nonce, cipher, tag, plain)) {
            return false;
        }

        outPlainLine.assign(reinterpret_cast<const char*>(plain.data()), reinterpret_cast<const char*>(plain.data() + plain.size()));
        return true;
    }

    void Clear(Session& session)
    {
        session.active = false;
        session.key.fill(0);
    }
}
