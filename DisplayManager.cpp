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
    std::vector<DisplayInfo> displays;
    DebugLog("GetDisplaysForGPU: Start (DXGI) - VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);

    IDXGIFactory* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        DebugLog("GetDisplaysForGPU: Failed to create DXGIFactory.");
        return displays;
    }

    IDXGIAdapter* pAdapter = nullptr;
    for (UINT ai = 0; pFactory->EnumAdapters(ai, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(pAdapter->GetDesc(&desc))) {
            pAdapter->Release();
            continue;
        }

        if (wcscmp(desc.Description, L"Microsoft Basic Render Driver") == 0) {
            pAdapter->Release();
            continue;
        }

        if (std::to_string(desc.VendorId) != gpuVendorID || std::to_string(desc.DeviceId) != gpuDeviceID) {
            pAdapter->Release();
            continue;
        }

        // Enumerate outputs in ascending index = port order
        IDXGIOutput* pOutput = nullptr;
        for (UINT oi = 0; pAdapter->EnumOutputs(oi, &pOutput) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC od{};
            if (FAILED(pOutput->GetDesc(&od))) {
                pOutput->Release();
                continue;
            }

            MONITORINFOEXW miex{};
            miex.cbSize = sizeof(miex);
            if (!GetMonitorInfoW(od.Monitor, &miex)) {
                DebugLog("GetDisplaysForGPU: GetMonitorInfoW failed for an output.");
                pOutput->Release();
                continue;
            }

            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            int devIndex = 0;
            bool matched = false;

            while (EnumDisplayDevicesW(NULL, devIndex, &dd, 0)) {
                if ((dd.StateFlags & DISPLAY_DEVICE_ACTIVE) && wcscmp(dd.DeviceName, miex.szDevice) == 0) {
                    DISPLAY_DEVICEW ddMon{};
                    ddMon.cb = sizeof(ddMon);
                    if (EnumDisplayDevicesW(dd.DeviceName, 0, &ddMon, 0)) {
                        DisplayInfo di{};
                        di.name = ConvertWStringToString(ddMon.DeviceID);
                        di.serialNumber = ConvertWStringToString(ddMon.DeviceID);
                        di.isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                        displays.push_back(di);
                        DebugLog("GetDisplaysForGPU: Found display (port order): " + di.serialNumber + ", IsPrimary: " + std::to_string(di.isPrimary));
                        matched = true;
                        break;
                    }
                }
                devIndex++;
            }
            pOutput->Release();
            if (!matched) {
                DebugLog("GetDisplaysForGPU: Output had no matching DISPLAY_DEVICE.");
            }
        }
        pAdapter->Release();
        break; // Found and handled the target adapter
    }
    pFactory->Release();

    if (displays.empty()) {
        DebugLog("GetDisplaysForGPU: No displays found for GPU VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    }

    return displays;
}




