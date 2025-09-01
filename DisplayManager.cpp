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


// NEW: Returns displays strictly in GPU port order using DXGI outputs.
std::vector<DisplayInfo> DisplayManager::GetDisplaysForGPUByPortOrder(const std::string& gpuVendorID, const std::string& gpuDeviceID) {
    std::vector<DisplayInfo> ordered;
    DebugLog("GetDisplaysForGPUByPortOrder: Start - VendorID=" + gpuVendorID + " DeviceID=" + gpuDeviceID);

    // 1. Get all displays for the target GPU, but unordered, and store them in a map.
    // The map key will be the DeviceName (e.g., L"\\\\.\\DISPLAY1").
    std::map<std::wstring, DisplayInfo> displaysOnGpuMap;
    DISPLAY_DEVICEW dd;
    dd.cb = sizeof(dd);
    int deviceIndex = 0;

    while (EnumDisplayDevicesW(NULL, deviceIndex, &dd, 0)) {
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
            // Check if this adapter matches the target GPU
            std::string deviceIDString = ConvertWStringToString(dd.DeviceID);
            std::regex vendorRegex("VEN_([0-9A-Fa-f]+)");
            std::regex deviceRegex("DEV_([0-9A-Fa-f]+)");
            std::smatch vendorMatch, deviceMatch;
            std::string extractedVendorID, extractedDeviceID;
            if (std::regex_search(deviceIDString, vendorMatch, vendorRegex) && vendorMatch.size() > 1) {
                extractedVendorID = vendorMatch.str(1);
            }
            if (std::regex_search(deviceIDString, deviceMatch, deviceRegex) && deviceMatch.size() > 1) {
                extractedDeviceID = deviceMatch.str(1);
            }

            unsigned int extractedVendorIDDec = 0, extractedDeviceIDDec = 0;
            if (!extractedVendorID.empty()) {
                std::stringstream ss;
                ss << std::hex << extractedVendorID;
                ss >> extractedVendorIDDec;
            }
            if (!extractedDeviceID.empty()) {
                std::stringstream ss;
                ss.clear();
                ss << std::hex << extractedDeviceID;
                ss >> extractedDeviceIDDec;
            }

            unsigned int targetVendorIDDec = 0, targetDeviceIDDec = 0;
            try {
                targetVendorIDDec = std::stoi(gpuVendorID);
                targetDeviceIDDec = std::stoi(gpuDeviceID);
            } catch (const std::exception& e) {
                DebugLog("GetDisplaysForGPUByPortOrder: Failed to convert GPU IDs to integers: " + std::string(e.what()));
                continue;
            }


            if (extractedVendorIDDec == targetVendorIDDec && extractedDeviceIDDec == targetDeviceIDDec) {
                // This adapter is the one we want. Get its monitors.
                DISPLAY_DEVICEW ddMonitor;
                ddMonitor.cb = sizeof(ddMonitor);
                if (EnumDisplayDevicesW(dd.DeviceName, 0, &ddMonitor, 0)) {
                    DisplayInfo di;
                    di.name = ConvertWStringToString(ddMonitor.DeviceID);
                    di.serialNumber = ConvertWStringToString(ddMonitor.DeviceID); // Brief says DeviceID is used for serial
                    di.isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                    displaysOnGpuMap[dd.DeviceName] = di;
                    DebugLog("GetDisplaysForGPUByPortOrder: Found potential display on correct GPU - DeviceName: " + ConvertWStringToString(dd.DeviceName) + ", Serial: " + di.serialNumber);
                }
            }
        }
        deviceIndex++;
    }


    IDXGIFactory* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        DebugLog("GetDisplaysForGPUByPortOrder: Failed to create DXGIFactory.");
        return ordered;
    }

    // Find the matching adapter
    IDXGIAdapter* pAdapter = nullptr;
    for (UINT ai = 0; pFactory->EnumAdapters(ai, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        DXGI_ADAPTER_DESC ad{};
        if (FAILED(pAdapter->GetDesc(&ad))) { pAdapter->Release(); continue; }

        if (std::to_string(ad.VendorId) == gpuVendorID && std::to_string(ad.DeviceId) == gpuDeviceID) {
            // Enumerate outputs in index order = port order
            for (UINT oi = 0; ; ++oi) {
                IDXGIOutput* pOut = nullptr;
                if (pAdapter->EnumOutputs(oi, &pOut) == DXGI_ERROR_NOT_FOUND) break;

                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(pOut->GetDesc(&od))) {
                    // Map this output to our existing display list entry by DeviceName (e.g., "\\\\.\\DISPLAY1")
                    auto it = displaysOnGpuMap.find(od.DeviceName);
                    if (it != displaysOnGpuMap.end()) {
                        ordered.push_back(it->second);
                        DebugLog("GetDisplaysForGPUByPortOrder: Mapped output " + std::to_string(oi) + " to display " + it->second.serialNumber);
                    }
                }
                pOut->Release();
            }
            pAdapter->Release();
            break;
        }
        pAdapter->Release();
    }
    pFactory->Release();

    if (ordered.empty()) {
        DebugLog("GetDisplaysForGPUByPortOrder: No displays found or failed to order them.");
    }
    return ordered;
}


std::vector<DisplayInfo> DisplayManager::GetDisplaysForGPU(const std::string& gpuVendorID, const std::string& gpuDeviceID) {
    std::vector<DisplayInfo> displays; // 修正: std::vector<DisplayInfo> に変更
    DebugLog("GetDisplaysForGPU: Start - VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);

    DISPLAY_DEVICE dd;
    dd.cb = sizeof(dd);
    int deviceIndex = 0;

    // 正規表現を使用してベンダーIDとデバイスIDを抽出
    std::regex vendorRegex("VEN_([0-9A-Fa-f]+)");
    std::regex deviceRegex("DEV_([0-9A-Fa-f]+)");

    while (EnumDisplayDevices(NULL, deviceIndex, &dd, 0)) {
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
            DISPLAY_DEVICE ddMonitor;
            ddMonitor.cb = sizeof(ddMonitor);
            if (EnumDisplayDevices(dd.DeviceName, 0, &ddMonitor, 0)) {
                // ディスプレイが繋がっているGPUのベンダーIDとデバイスIDを取得
                std::string deviceID = ConvertWStringToString(dd.DeviceID);

                std::smatch vendorMatch, deviceMatch;

                std::string extractedVendorID, extractedDeviceID;
                if (std::regex_search(deviceID, vendorMatch, vendorRegex) && vendorMatch.size() > 1) {
                    extractedVendorID = vendorMatch.str(1);
                }
                if (std::regex_search(deviceID, deviceMatch, deviceRegex) && deviceMatch.size() > 1) {
                    extractedDeviceID = deviceMatch.str(1);
                }

                // 16進数の文字列を10進数に変換
                unsigned int extractedVendorIDDec = 0, extractedDeviceIDDec = 0;
                std::stringstream ss;
                ss << std::hex << extractedVendorID;
                ss >> extractedVendorIDDec;
                ss.clear();
                ss << std::hex << extractedDeviceID;
                ss >> extractedDeviceIDDec;

                unsigned int gpuVendorIDDec = 0, gpuDeviceIDDec = 0;
                try {
                    gpuVendorIDDec = std::stoi(gpuVendorID);
                    gpuDeviceIDDec = std::stoi(gpuDeviceID);
                }
                catch (const std::exception& e) {
                    DebugLog("GetDisplaysForGPU: Failed to convert GPU IDs to integers: " + std::string(e.what()));
                    continue;
                }

                // ベンダーIDとデバイスIDが一致するか確認
                if (extractedVendorIDDec == gpuVendorIDDec && extractedDeviceIDDec == gpuDeviceIDDec) {
                    DisplayInfo di;
                    di.name = ConvertWStringToString(ddMonitor.DeviceID);
                    di.serialNumber = ConvertWStringToString(ddMonitor.DeviceID);
                    di.isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                    displays.push_back(di); // 修正: DisplayInfo を追加
                    DebugLog("GetDisplaysForGPU: Found display - Name: " + di.name + ", SerialNumber: " + di.serialNumber + ", IsPrimary: " + std::to_string(di.isPrimary));
                }
            }
            else {
                DebugLog("GetDisplaysForGPU: Failed to enumerate display devices for " + ConvertWStringToString(dd.DeviceName));
            }
        }
        deviceIndex++;
    }

    if (displays.empty()) {
        DebugLog("GetDisplaysForGPU: No displays found for GPU VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    }

    return displays;
}




