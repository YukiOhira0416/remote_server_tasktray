#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "DeviceSignKey.h"

#include "hk_tweetnacl_sign.h"

#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <fstream>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace DeviceSignKey
{
    namespace
    {
        static void SetError(std::string* outError, const char* msg)
        {
            if (outError) {
                *outError = msg ? msg : "";
            }
        }

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

        static std::wstring KeyFilePath()
        {
            std::wstring base = GetProgramDataPath();
            std::wstring dir = base + L"\\HayateKomorebi\\DeviceKeys";
            EnsureDirExists(dir);
            return dir + L"\\device_ed25519_sign.key";
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

        static bool DpapiProtectMachine(const std::vector<uint8_t>& plain, std::vector<uint8_t>& out)
        {
            out.clear();
            DATA_BLOB in{};
            in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plain.data()));
            in.cbData = static_cast<DWORD>(plain.size());

            DATA_BLOB outBlob{};
            if (!CryptProtectData(&in, L"hk_ed25519_sign", nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE, &outBlob)) {
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

        static bool Base64EncodeNoCrlf(const uint8_t* data, DWORD len, std::string& out)
        {
            out.clear();
            DWORD needed = 0;
            if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed)) {
                return false;
            }
            std::string tmp;
            tmp.resize(needed);
            if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, tmp.data(), &needed)) {
                return false;
            }
            if (!tmp.empty() && tmp.back() == '\0') {
                tmp.pop_back();
            }
            out.swap(tmp);
            return true;
        }

        static bool LoadOrCreateSecretKey(std::array<uint8_t, 64>& outSk, std::array<uint8_t, 32>& outPk, std::string* outError)
        {
            outSk.fill(0);
            outPk.fill(0);

            const std::wstring path = KeyFilePath();
            std::vector<uint8_t> file;
            if (ReadFileAll(path, file)) {
                if (file.size() >= 9 && std::memcmp(file.data(), "HKED1", 5) == 0) {
                    uint32_t encLen = 0;
                    std::memcpy(&encLen, file.data() + 5, 4);
                    if (9ull + encLen == file.size() && encLen > 0) {
                        std::vector<uint8_t> enc(file.begin() + 9, file.end());
                        std::vector<uint8_t> sk;
                        if (DpapiUnprotectMachine(enc, sk) && sk.size() == 64) {
                            std::memcpy(outSk.data(), sk.data(), 64);
                            std::memcpy(outPk.data(), outSk.data() + 32, 32);
                            SecureZeroMemory(sk.data(), sk.size());
                            return true;
                        }
                    }
                }
            }

            // Create new key.
            if (crypto_sign_keypair(outPk.data(), outSk.data()) != 0) {
                SetError(outError, "crypto_sign_keypair failed");
                return false;
            }

            std::vector<uint8_t> plain(outSk.begin(), outSk.end());
            std::vector<uint8_t> enc;
            if (!DpapiProtectMachine(plain, enc)) {
                SecureZeroMemory(plain.data(), plain.size());
                SetError(outError, "DPAPI protect failed");
                return false;
            }
            SecureZeroMemory(plain.data(), plain.size());

            std::vector<uint8_t> outFile;
            outFile.insert(outFile.end(), {'H','K','E','D','1'});
            uint32_t encLen = static_cast<uint32_t>(enc.size());
            outFile.insert(outFile.end(), reinterpret_cast<uint8_t*>(&encLen), reinterpret_cast<uint8_t*>(&encLen) + 4);
            outFile.insert(outFile.end(), enc.begin(), enc.end());

            (void)WriteFileAll(path, outFile);
            return true;
        }
    }

    bool GetOrCreatePublicKeyBase64(std::string& outPublicKeyB64, std::string* outError)
    {
        outPublicKeyB64.clear();
        std::array<uint8_t, 64> sk{};
        std::array<uint8_t, 32> pk{};
        if (!LoadOrCreateSecretKey(sk, pk, outError)) {
            return false;
        }

        bool ok = Base64EncodeNoCrlf(pk.data(), (DWORD)pk.size(), outPublicKeyB64);
        SecureZeroMemory(sk.data(), sk.size());
        if (!ok) {
            SetError(outError, "base64 encode failed");
        }
        return ok;
    }

    bool SignDetachedBase64(const std::string& messageUtf8, std::string& outSignatureB64, std::string* outError)
    {
        outSignatureB64.clear();

        std::array<uint8_t, 64> sk{};
        std::array<uint8_t, 32> pk{};
        if (!LoadOrCreateSecretKey(sk, pk, outError)) {
            return false;
        }

        const uint8_t* m = reinterpret_cast<const uint8_t*>(messageUtf8.data());
        uint64_t mlen = static_cast<uint64_t>(messageUtf8.size());

        // crypto_sign outputs sm = sig||m
        std::vector<uint8_t> sm;
        sm.resize(static_cast<size_t>(mlen + 64));
        uint64_t smlen = 0;
        if (crypto_sign(sm.data(), &smlen, m, mlen, sk.data()) != 0 || smlen != mlen + 64) {
            SecureZeroMemory(sk.data(), sk.size());
            SetError(outError, "crypto_sign failed");
            return false;
        }

        bool ok = Base64EncodeNoCrlf(sm.data(), 64, outSignatureB64);

        SecureZeroMemory(sk.data(), sk.size());
        SecureZeroMemory(sm.data(), sm.size());

        if (!ok) {
            SetError(outError, "base64 encode failed");
            return false;
        }

        return true;
    }

    bool DeleteStoredKey(std::string* outError)
    {
        const std::wstring path = KeyFilePath();
        if (DeleteFileW(path.c_str()) != 0) {
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        SetError(outError, "DeleteFile failed");
        return false;
    }
}
