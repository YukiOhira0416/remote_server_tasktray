#include "RegistryHelper.h"
#include "Utility.h"
#include <windows.h>
#include <stringapiset.h>
#include "DebugLog.h"
#include "Globals.h"
#include <mutex>

// 排他処理用のミューテックス
static std::mutex registryMutex;

bool RegistryHelper::WriteRegistry(const std::string& vendorID, const std::string& deviceID) {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_GPU, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring wVendorID = utf8_to_utf16(vendorID);
        std::wstring wDeviceID = utf8_to_utf16(deviceID);
        if (RegSetValueEx(hKey, L"VendorID", 0, REG_SZ, (BYTE*)wVendorID.c_str(), static_cast<DWORD>((wVendorID.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            DebugLog("WriteRegistry: Failed to write VendorID to registry.");
            RegCloseKey(hKey);
            return false;
        }
        if (RegSetValueEx(hKey, L"DeviceID", 0, REG_SZ, (BYTE*)wDeviceID.c_str(), static_cast<DWORD>((wDeviceID.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            DebugLog("WriteRegistry: Failed to write DeviceID to registry.");
            RegCloseKey(hKey);
            return false;
        }
        RegCloseKey(hKey);
        DebugLog("WriteRegistry: Successfully wrote VendorID and DeviceID to registry.");
        return true;
    }
    else {
        DebugLog("WriteRegistry: Failed to create or open registry key.");
    }
    return false;
}

std::pair<std::string, std::string> RegistryHelper::ReadRegistry() {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    wchar_t vendorID[256], deviceID[256];
    DWORD vendorSize = sizeof(vendorID);
    DWORD deviceSize = sizeof(deviceID);

    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_GPU, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, L"VendorID", NULL, NULL, (LPBYTE)vendorID, &vendorSize) != ERROR_SUCCESS) {
            DebugLog("ReadRegistry: Failed to read VendorID from registry.");
            RegCloseKey(hKey);
            return { "", "" };
        }
        if (RegQueryValueEx(hKey, L"DeviceID", NULL, NULL, (LPBYTE)deviceID, &deviceSize) != ERROR_SUCCESS) {
            DebugLog("ReadRegistry: Failed to read DeviceID from registry.");
            RegCloseKey(hKey);
            return { "", "" };
        }
        RegCloseKey(hKey);
        DebugLog("ReadRegistry: Successfully read VendorID and DeviceID from registry.");
        return { utf16_to_utf8(vendorID), utf16_to_utf8(deviceID) };
    }
    else {
        DebugLog("ReadRegistry: Failed to open registry key.");
    }
    return { "", "" };
}

bool RegistryHelper::WriteDISPInfoToRegistry(const std::string& DISPInfo) {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {

        // キー名を作成
        std::wstring keyName = L"SerialNumber" + std::to_wstring(serialNumberIndex);
        std::wstring wDISPInfo = utf8_to_utf16(DISPInfo);

        // レジストリに書き込む
        if (RegSetValueEx(hKey, keyName.c_str(), 0, REG_SZ, (BYTE*)wDISPInfo.c_str(), static_cast<DWORD>((wDISPInfo.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            DebugLog("WriteDISPInfoToRegistry: Failed to write SerialNumber to registry.");
            RegCloseKey(hKey);
            return false;
        }

        // 連番をインクリメント
        serialNumberIndex++;

        RegCloseKey(hKey);
        DebugLog("WriteDISPInfoToRegistry: Successfully wrote SerialNumber to registry.");
        return true;
    }
    else {
        DebugLog("WriteDISPInfoToRegistry: Failed to create or open registry key.");
        return false;
    }
}

std::vector<std::string> RegistryHelper::ReadDISPInfoFromRegistry() {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    std::vector<std::string> serialNumbers;

    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD valueCount = 0;
        RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &valueCount, NULL, NULL, NULL, NULL);

        for (DWORD i = 0; i < valueCount; ++i) {
            wchar_t valueName[256];
            DWORD valueNameSize = sizeof(valueName) / sizeof(valueName[0]);
            wchar_t serialNumber[256];
            DWORD serialSize = sizeof(serialNumber);

            if (RegEnumValue(hKey, i, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                if (RegQueryValueEx(hKey, valueName, NULL, NULL, (LPBYTE)serialNumber, &serialSize) == ERROR_SUCCESS) {
                    serialNumbers.push_back(utf16_to_utf8(serialNumber));
                }
            }
        }

        RegCloseKey(hKey);
        DebugLog("ReadDISPInfoFromRegistry: Successfully read SerialNumbers from registry.");
    }
    else {
        DebugLog("ReadDISPInfoFromRegistry: Failed to open registry key.");
    }

    return serialNumbers;
}

bool RegistryHelper::WriteSelectedDisplaySerial(const std::string& serial) {
    std::lock_guard<std::mutex> lock(registryMutex);
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring w = utf8_to_utf16(serial);
        LONG rc = RegSetValueEx(hKey, L"SelectedSerial", 0, REG_SZ,
                                (BYTE*)w.c_str(), static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
        if (rc == ERROR_SUCCESS) {
            DebugLog("WriteSelectedDisplaySerial: OK");
            return true;
        }
        DebugLog("WriteSelectedDisplaySerial: Failed");
    }
    else {
        DebugLog("WriteSelectedDisplaySerial: Open key failed");
    }
    return false;
}

std::string RegistryHelper::ReadSelectedDisplaySerial() {
    std::lock_guard<std::mutex> lock(registryMutex);
    HKEY hKey;
    wchar_t buf[512];
    DWORD sz = sizeof(buf);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, L"SelectedSerial", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return utf16_to_utf8(buf);
        }
        RegCloseKey(hKey);
    }
    return "";
}