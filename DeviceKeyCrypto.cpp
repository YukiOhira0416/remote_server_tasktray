#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "DeviceKeyCrypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace DeviceKeyCrypto {

namespace {

struct ScopedAlg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~ScopedAlg() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};
struct ScopedKey {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~ScopedKey() { if (h) BCryptDestroyKey(h); }
};

static std::wstring GetProgramDataPath()
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::wstring(buf);
    }
    return L"C:\\ProgramData";
}

static void EnsureDirExists(const std::wstring& dir)
{
    CreateDirectoryW(dir.c_str(), nullptr);
}

static std::wstring GetKeyFilePath()
{
    std::wstring base = GetProgramDataPath();
    std::wstring root = base + L"\\HayateKomorebi";
    EnsureDirExists(root);
    std::wstring dir = root + L"\\DeviceKeys";
    EnsureDirExists(dir);
    return dir + L"\\server_ecdh_p256.key";
}

static bool ReadAllBytes(const std::wstring& path, std::vector<uint8_t>& out)
{
    out.clear();
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    std::streamsize sz = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    if (sz <= 0) return false;
    out.resize(static_cast<size_t>(sz));
    if (!ifs.read(reinterpret_cast<char*>(out.data()), sz)) return false;
    return true;
}

static bool WriteAllBytes(const std::wstring& path, const std::vector<uint8_t>& data)
{
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(ofs);
}

static bool DpapiProtectLocalMachine(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    out.clear();
    DATA_BLOB inBlob{ (DWORD)in.size(), (BYTE*)in.data() };
    DATA_BLOB outBlob{};
    if (!CryptProtectData(&inBlob, L"HKDK", nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE, &outBlob)) {
        return false;
    }
    out.assign(outBlob.pbData, outBlob.pbData + outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

static bool DpapiUnprotectLocalMachine(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    out.clear();
    DATA_BLOB inBlob{ (DWORD)in.size(), (BYTE*)in.data() };
    DATA_BLOB outBlob{};
    if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE, &outBlob)) {
        return false;
    }
    out.assign(outBlob.pbData, outBlob.pbData + outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

static bool Base64EncodeNoCrlf(const uint8_t* data, DWORD len, std::string& out)
{
    out.clear();
    DWORD cch = 0;
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &cch)) {
        return false;
    }
    std::string tmp;
    tmp.resize(cch);
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, tmp.data(), &cch)) {
        return false;
    }
    if (!tmp.empty() && tmp.back() == '\0') tmp.pop_back();
    out = std::move(tmp);
    return true;
}

static bool LoadOrCreateKeyPair(ScopedAlg& alg, ScopedKey& key, std::vector<uint8_t>& pubBlob)
{
    pubBlob.clear();

    if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }

    const std::wstring path = GetKeyFilePath();
    std::vector<uint8_t> file;
    if (ReadAllBytes(path, file) && file.size() >= 8 && std::memcmp(file.data(), "HKDK", 4) == 0) {
        uint32_t len = 0;
        std::memcpy(&len, file.data() + 4, 4);
        if (8ull + len == file.size()) {
            std::vector<uint8_t> protectedBlob(file.begin() + 8, file.end());
            std::vector<uint8_t> priv;
            if (DpapiUnprotectLocalMachine(protectedBlob, priv)) {
                if (BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key.h,
                                        priv.data(), (ULONG)priv.size(), 0) == 0) {
                    ULONG cbPub = 0;
                    if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &cbPub, 0) == 0 && cbPub > 0) {
                        pubBlob.resize(cbPub);
                        if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, pubBlob.data(), cbPub, &cbPub, 0) == 0) {
                            pubBlob.resize(cbPub);
                            return true;
                        }
                    }
                }
            }
        }
    }

    if (BCryptGenerateKeyPair(alg.h, &key.h, 256, 0) != 0) {
        return false;
    }
    if (BCryptFinalizeKeyPair(key.h, 0) != 0) {
        return false;
    }

    ULONG cbPriv = 0;
    if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &cbPriv, 0) != 0 || cbPriv == 0) {
        return false;
    }
    std::vector<uint8_t> priv(cbPriv);
    if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, priv.data(), cbPriv, &cbPriv, 0) != 0) {
        return false;
    }
    priv.resize(cbPriv);

    ULONG cbPub = 0;
    if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &cbPub, 0) != 0 || cbPub == 0) {
        return false;
    }
    pubBlob.resize(cbPub);
    if (BCryptExportKey(key.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, pubBlob.data(), cbPub, &cbPub, 0) != 0) {
        return false;
    }
    pubBlob.resize(cbPub);

    std::vector<uint8_t> protectedBlob;
    if (DpapiProtectLocalMachine(priv, protectedBlob)) {
        std::vector<uint8_t> outFile;
        outFile.insert(outFile.end(), {'H','K','D','K'});
        uint32_t len = (uint32_t)protectedBlob.size();
        outFile.insert(outFile.end(), (uint8_t*)&len, (uint8_t*)&len + 4);
        outFile.insert(outFile.end(), protectedBlob.begin(), protectedBlob.end());
        (void)WriteAllBytes(path, outFile);
    }

    return true;
}

} // namespace

bool GetServerPublicKeyForDb(std::string& outPublicKeyPem)
{
    outPublicKeyPem.clear();

    ScopedAlg alg;
    ScopedKey key;
    std::vector<uint8_t> pub;
    if (!LoadOrCreateKeyPair(alg, key, pub)) {
        return false;
    }

    std::string b64;
    if (!Base64EncodeNoCrlf(pub.data(), (DWORD)pub.size(), b64)) {
        return false;
    }

    outPublicKeyPem = std::string("HKP1:") + b64;
    return true;
}

} // namespace DeviceKeyCrypto
