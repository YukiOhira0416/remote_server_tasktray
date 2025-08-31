#include "DisplayManager.h"
#include "StringConversion.h"
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
#include <tchar.h>
#include "SharedMemoryHelper.h"
#include "GPUManager.h"
#include <cstring>
#include <functional>
#include <algorithm>
#include <map>
#include "Utility.h"
#include <dxgi.h>
#include <d3d11.h>
#include <regex>
#include <sstream>
#include "GPUInfo.h"
#include "DebugLog.h" // 追加


#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

std::vector<GPUInfo> DisplayManager::GetInstalledGPUs() {
    std::vector<GPUInfo> gpus;
    IDXGIFactory* pFactory;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        DebugLog("GetInstalledGPUs: Failed to create DXGIFactory.");
        return gpus;
    }

    IDXGIAdapter* pAdapter;
    for (UINT i = 0; pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc;
        if (FAILED(pAdapter->GetDesc(&desc))) {
            DebugLog("GetInstalledGPUs: Failed to get adapter description.");
            pAdapter->Release();
            continue;
        }

        // Microsoft Basic Render Driver を除外
        if (wcscmp(desc.Description, L"Microsoft Basic Render Driver") == 0) {
            pAdapter->Release();
            continue;
        }

        GPUInfo gpu;
        gpu.vendorID = std::to_string(desc.VendorId);
        gpu.deviceID = std::to_string(desc.DeviceId);
        gpu.name = WideStringToMultiByte(desc.Description);

        // ハードウェアエンコードサポートを確認
        gpu.supportsHardwareEncoding = CheckHardwareEncodingSupport(pAdapter);

        gpus.push_back(gpu);
        pAdapter->Release();
    }
    pFactory->Release();
    return gpus;
}

bool DisplayManager::CheckHardwareEncodingSupport(IDXGIAdapter* pAdapter) {
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        pAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &pDevice,
        &featureLevel,
        &pContext
    );

    if (FAILED(hr)) {
        DebugLog("CheckHardwareEncodingSupport: Failed to create D3D11 device.");
        return false;
    }

    // ハードウェアエンコードサポートを確認するためのコードを追加
    // 例: D3D11_FEATURE_DATA_D3D11_OPTIONS を使用して確認
    D3D11_FEATURE_DATA_D3D11_OPTIONS options;
    hr = pDevice->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
    if (FAILED(hr)) {
        DebugLog("CheckHardwareEncodingSupport: Failed to check feature support.");
        pDevice->Release();
        pContext->Release();
        return false;
    }

    bool supportsHardwareEncoding = options.OutputMergerLogicOp; // 仮のチェック項目
    pDevice->Release();
    pContext->Release();
    return supportsHardwareEncoding;
}


std::vector<DisplayInfo> DisplayManager::GetDisplaysForGPU(const std::string& gpuVendorID, const std::string& gpuDeviceID) {
    std::vector<DisplayInfo> displays;
    DebugLog("GetDisplaysForGPU: Start - VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);

    // Create a map to store port indices from QueryDisplayConfig
    std::map<std::wstring, int> portIndices;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &pathCount, &modeCount) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < pathCount; ++i) {
                if (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) {
                    DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
                    targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    targetName.header.size = sizeof(targetName);
                    targetName.header.adapterId = paths[i].targetInfo.adapterId;
                    targetName.header.id = paths[i].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS) {
                        // Use index i as a stable port index. The key is the unique monitor device path.
                        portIndices[targetName.monitorDevicePath] = i;
                    }
                }
            }
        }
    }

    DISPLAY_DEVICE ddAdapter;
    ddAdapter.cb = sizeof(ddAdapter);
    int adapterIndex = 0;

    std::regex vendorRegex("VEN_([0-9A-Fa-f]+)");
    std::regex deviceRegex("DEV_([0-9A-Fa-f]+)");

    while (EnumDisplayDevices(NULL, adapterIndex, &ddAdapter, 0)) {
        if (!(ddAdapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            adapterIndex++;
            continue;
        }

        std::string adapterDeviceIDStr = ConvertWStringToString(ddAdapter.DeviceID);
        std::smatch vendorMatch, deviceMatch;
        std::string extractedVendorID, extractedDeviceID;
        if (std::regex_search(adapterDeviceIDStr, vendorMatch, vendorRegex) && vendorMatch.size() > 1) {
            extractedVendorID = vendorMatch.str(1);
        }
        if (std::regex_search(adapterDeviceIDStr, deviceMatch, deviceRegex) && deviceMatch.size() > 1) {
            extractedDeviceID = deviceMatch.str(1);
        }

        unsigned int extractedVendorIDDec = 0, extractedDeviceIDDec = 0;
        try {
            if (!extractedVendorID.empty()) extractedVendorIDDec = std::stoul(extractedVendorID, nullptr, 16);
            if (!extractedDeviceID.empty()) extractedDeviceIDDec = std::stoul(extractedDeviceID, nullptr, 16);
        } catch (...) { /* ignore conversion errors */ }

        unsigned int targetVendorIDDec = 0, targetDeviceIDDec = 0;
        try {
            targetVendorIDDec = std::stoi(gpuVendorID);
            targetDeviceIDDec = std::stoi(gpuDeviceID);
        } catch (const std::exception& e) {
            DebugLog("GetDisplaysForGPU: Failed to convert target GPU IDs to integers: " + std::string(e.what()));
            adapterIndex++;
            continue;
        }

        if (extractedVendorIDDec == targetVendorIDDec && extractedDeviceIDDec == targetDeviceIDDec) {
            int monitorIndex = 0;
            DISPLAY_DEVICE ddMonitor;
            ddMonitor.cb = sizeof(ddMonitor);
            while (EnumDisplayDevices(ddAdapter.DeviceName, monitorIndex, &ddMonitor, 0)) {
                if (ddMonitor.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                    DisplayInfo di;
                    di.name = ConvertWStringToString(ddMonitor.DeviceString);
                    di.serialNumber = ConvertWStringToString(ddMonitor.DeviceID);
                    di.isPrimary = (ddAdapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

                    // Determine port index using QueryDisplayConfig results
                    std::wstring deviceIdWStr = utf8_to_utf16(di.serialNumber);
                    auto it = portIndices.find(deviceIdWStr);
                    if (it != portIndices.end()) {
                        di.portIndex = it->second;
                    } else {
                        // Fallback: parse from ddMonitor.DeviceName (e.g., \\.\DISPLAY1\Monitor0)
                        std::wstring monitorDeviceName(ddMonitor.DeviceName);
                        size_t lastDigit = monitorDeviceName.find_last_of(L"0123456789");
                        if (lastDigit != std::wstring::npos) {
                            try {
                                di.portIndex = 100 + std::stoi(monitorDeviceName.substr(lastDigit)); // Add offset to avoid collision
                            } catch (...) {
                                di.portIndex = 999; // Should not happen
                            }
                        } else {
                            di.portIndex = 999; // Could not determine
                        }
                    }
                    displays.push_back(di);
                    DebugLog("GetDisplaysForGPU: Found display - Name: " + di.name + ", Serial: " + di.serialNumber + ", Primary: " + std::to_string(di.isPrimary) + ", Port: " + std::to_string(di.portIndex));
                }
                monitorIndex++;
            }
        }
        adapterIndex++;
    }

    // Sort displays: primary first, then by port index
    std::sort(displays.begin(), displays.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        if (a.isPrimary != b.isPrimary) {
            return a.isPrimary > b.isPrimary;
        }
        return a.portIndex < b.portIndex;
    });

    if (displays.empty()) {
        DebugLog("GetDisplaysForGPU: No displays found for GPU VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    } else {
        DebugLog("GetDisplaysForGPU: Sorted display list. Primary is: " + displays[0].serialNumber);
    }

    return displays;
}




