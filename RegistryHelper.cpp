#include "RegistryHelper.h"
#include "Utility.h"
#include <windows.h>
#include <stringapiset.h>
#include "DebugLog.h"
#include "Globals.h"
#include <mutex>

#include <vector>

// Mutex for thread-safe registry access
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

bool RegistryHelper::WriteDISPInfoToRegistryAt(int index, const std::string& serial) {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        // Key name is "SerialNumber" + 1-based index.
        std::wstring keyName = L"SerialNumber" + std::to_wstring(index);
        std::wstring wSerial = utf8_to_utf16(serial);

        if (RegSetValueEx(hKey, keyName.c_str(), 0, REG_SZ, (BYTE*)wSerial.c_str(), static_cast<DWORD>((wSerial.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            DebugLog("WriteDISPInfoToRegistryAt: Failed to write SerialNumber to registry for index " + std::to_string(index));
            RegCloseKey(hKey);
            return false;
        }

        RegCloseKey(hKey);
        DebugLog("WriteDISPInfoToRegistryAt: Successfully wrote SerialNumber " + serial + " to index " + std::to_string(index));
        return true;
    }
    else {
        DebugLog("WriteDISPInfoToRegistryAt: Failed to create or open registry key.");
        return false;
    }
}

std::vector<std::string> RegistryHelper::ReadDISPInfoFromRegistry() {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    std::vector<std::string> serialNumbers;

    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        for (int i = 1; ; ++i) {
            std::wstring keyName = L"SerialNumber" + std::to_wstring(i);
            wchar_t serialNumber[256];
            DWORD serialSize = sizeof(serialNumber);

            LSTATUS status = RegQueryValueEx(hKey, keyName.c_str(), NULL, NULL, (LPBYTE)serialNumber, &serialSize);

            if (status == ERROR_SUCCESS) {
                serialNumbers.push_back(utf16_to_utf8(serialNumber));
            }
            else if (status == ERROR_FILE_NOT_FOUND) {
                // This is the termination condition: we've read all sequential keys.
                break;
            }
            else {
                // Some other error occurred.
                DebugLog("ReadDISPInfoFromRegistry: Error reading registry value for key " + utf16_to_utf8(keyName));
                break;
            }
        }
        RegCloseKey(hKey);
        DebugLog("ReadDISPInfoFromRegistry: Successfully read " + std::to_string(serialNumbers.size()) + " serial numbers from registry.");
    }
    else {
        DebugLog("ReadDISPInfoFromRegistry: Failed to open registry key.");
    }

    return serialNumbers;
}

bool RegistryHelper::WriteSelectedSerialToRegistry(const std::string& serial) {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring wSerial = utf8_to_utf16(serial);
        if (RegSetValueEx(hKey, L"SelectedSerial", 0, REG_SZ, (BYTE*)wSerial.c_str(), static_cast<DWORD>((wSerial.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            DebugLog("WriteSelectedSerialToRegistry: Failed to write SelectedSerial to registry.");
            RegCloseKey(hKey);
            return false;
        }
        RegCloseKey(hKey);
        DebugLog("WriteSelectedSerialToRegistry: Successfully wrote SelectedSerial: " + serial);
        return true;
    }
    else {
        DebugLog("WriteSelectedSerialToRegistry: Failed to create or open registry key.");
        return false;
    }
}

std::string RegistryHelper::ReadSelectedSerialFromRegistry() {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    std::string selectedSerial = "";

    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t serialBuffer[256];
        DWORD bufferSize = sizeof(serialBuffer);
        if (RegQueryValueEx(hKey, L"SelectedSerial", NULL, NULL, (LPBYTE)serialBuffer, &bufferSize) == ERROR_SUCCESS) {
            selectedSerial = utf16_to_utf8(serialBuffer);
        }
        else {
            DebugLog("ReadSelectedSerialFromRegistry: Failed to read SelectedSerial from registry or it does not exist.");
        }
        RegCloseKey(hKey);
    }
    else {
        DebugLog("ReadSelectedSerialFromRegistry: Failed to open registry key.");
    }
    return selectedSerial;
}

bool RegistryHelper::ClearDISPInfoFromRegistry() {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, KEY_ALL_ACCESS, &hKey) != ERROR_SUCCESS) {
        DebugLog("ClearDISPInfoFromRegistry: Key does not exist or cannot be opened, nothing to clear.");
        return true;
    }

    // To avoid issues with enumeration while deleting, get all value names first.
    std::vector<std::wstring> valueNames;
    DWORD i = 0;
    wchar_t valueName[256];
    DWORD valueNameSize = _countof(valueName);
    while (RegEnumValue(hKey, i++, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        valueNames.push_back(valueName);
        valueNameSize = _countof(valueName); // Reset size for next call
    }

    bool all_deleted = true;
    for (const auto& name : valueNames) {
        if (RegDeleteValue(hKey, name.c_str()) != ERROR_SUCCESS) {
            DebugLog("ClearDISPInfoFromRegistry: Failed to delete registry value: " + utf16_to_utf8(name));
            all_deleted = false;
        }
    }

    RegCloseKey(hKey);
    if (all_deleted) {
        DebugLog("ClearDISPInfoFromRegistry: Successfully cleared all display info values.");
    }
    return all_deleted;
}

bool RegistryHelper::WriteCaptureTypeToRegistry(const std::string& CaptureType) {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_CAPTURE_TYPE, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring wCaptureType = utf8_to_utf16(CaptureType);
        if (RegSetValueEx(hKey, L"CaptureType", 0, REG_SZ, (BYTE*)wCaptureType.c_str(), static_cast<DWORD>((wCaptureType.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            DebugLog("WriteRegistry: Failed to write CaptureType to registry.");
            RegCloseKey(hKey);
            return false;
        }
        RegCloseKey(hKey);
        DebugLog("WriteRegistry: Successfully wrote CaptureType to registry.");
        return true;
    }
    else {
        DebugLog("WriteRegistry: Failed to create or open registry key.");
    }
    return false;
}


std::string RegistryHelper::ReadCaptureTypeFromRegistry() {
    std::lock_guard<std::mutex> lock(registryMutex);

    HKEY hKey;
    wchar_t capture_type[256] = {0};
    DWORD capture_type_Size = sizeof(capture_type);

    std::string captureInfo = "";

    // MACHINE_INFOからGPU情報を読み取り
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_CAPTURE_TYPE, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, L"CaptureType", NULL, NULL, (LPBYTE)capture_type, &capture_type_Size) == ERROR_SUCCESS) {
            captureInfo = utf16_to_utf8(capture_type);
        }
        RegCloseKey(hKey);
    }
    return captureInfo;
}