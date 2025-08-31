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

            unsigned int extractedVendorIDDec = 0, extractedDeviceIDDec = 0;
            std::stringstream ss;
            ss << std::hex << extractedVendorID; ss >> extractedVendorIDDec; ss.clear();
            ss << std::hex << extractedDeviceID; ss >> extractedDeviceIDDec;

            unsigned int gpuVendorIDDec = 0, gpuDeviceIDDec = 0;
            try {
                gpuVendorIDDec = std::stoi(gpuVendorID);
                gpuDeviceIDDec = std::stoi(gpuDeviceID);
            }
            catch (const std::exception& e) {
                DebugLog("GetDisplaysForGPU: Failed to convert GPU IDs to integers: " + std::string(e.what()));
                continue;
            }

            if (extractedVendorIDDec == gpuVendorIDDec && extractedDeviceIDDec == gpuDeviceIDDec) {
                DISPLAY_DEVICE ddMonitor;
                for (DWORD j = 0; ; ++j) {
                    ZeroMemory(&ddMonitor, sizeof(ddMonitor));
                    ddMonitor.cb = sizeof(ddMonitor);
                    if (!EnumDisplayDevices(dd.DeviceName, j, &ddMonitor, 0)) {
                        break;
                    }

                    if (ddMonitor.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                        DisplayInfo di;
                        di.name = ConvertWStringToString(ddMonitor.DeviceID);
                        di.serialNumber = ConvertWStringToString(ddMonitor.DeviceID);
                        di.isPrimary = ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) && (j == 0);
                        displays.push_back(di);
                        DebugLog("GetDisplaysForGPU: Found display - Name: " + di.name + ", SerialNumber: " + di.serialNumber + ", IsPrimary: " + std::to_string(di.isPrimary));
                    }
                }
            }
        }
        deviceIndex++;
    }

    if (displays.empty()) {
        DebugLog("GetDisplaysForGPU: No displays found for GPU VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    }

    return displays;
}




