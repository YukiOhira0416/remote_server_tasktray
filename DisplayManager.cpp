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
    DebugLog("GetDisplaysForGPU(DXGI): Start - VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);

    IDXGIFactory* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        DebugLog("GetDisplaysForGPU(DXGI): Failed to create DXGIFactory.");
        return displays;
    }

    IDXGIAdapter* pAdapter = nullptr;
    HRESULT hr = S_OK;
    for (UINT ai = 0; (hr = pFactory->EnumAdapters(ai, &pAdapter)) != DXGI_ERROR_NOT_FOUND; ++ai) {
        DXGI_ADAPTER_DESC ad = {};
        if (FAILED(pAdapter->GetDesc(&ad))) { pAdapter->Release(); continue; }

        // Match by decimal VendorId/DeviceId (your GPU_INFO stores decimal strings)
        if (std::to_string(ad.VendorId) != gpuVendorID || std::to_string(ad.DeviceId) != gpuDeviceID) {
            pAdapter->Release();
            continue;
        }

        // Enumerate outputs in adapter order (this is the GPU's "port order").
        std::vector<DisplayInfo> portOrder;
        IDXGIOutput* pOutput = nullptr;
        for (UINT oi = 0; pAdapter->EnumOutputs(oi, &pOutput) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC od = {};
            if (FAILED(pOutput->GetDesc(&od))) { pOutput->Release(); continue; }
            if (!od.AttachedToDesktop) { pOutput->Release(); continue; } // active only

            MONITORINFO mi = { sizeof(MONITORINFO) };
            if (!GetMonitorInfo(od.Monitor, &mi)) { pOutput->Release(); continue; }

            DisplayInfo di;
            // Keep using device identifier semantics similar to existing code.
            // If you later want EDID, do it as a separate, permissioned change.
            di.name = ConvertWStringToString(od.DeviceName);
            di.serialNumber = ConvertWStringToString(od.DeviceName);
            di.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            portOrder.push_back(di);
            pOutput->Release();
        }

        // Reorder: primary first, then remaining in original port order
        auto it = std::find_if(portOrder.begin(), portOrder.end(),
                               [](const DisplayInfo& d){ return d.isPrimary; });
        if (it != portOrder.end()) {
            displays.push_back(*it);
            for (size_t i = 0; i < portOrder.size(); ++i) {
                if (!portOrder[i].isPrimary) displays.push_back(portOrder[i]);
            }
        } else {
            // No primary? Fallback to port order as-is.
            displays = std::move(portOrder);
        }

        pAdapter->Release();
        break; // matched adapter handled; stop
    }

    pFactory->Release();

    if (displays.empty()) {
        DebugLog("GetDisplaysForGPU(DXGI): No active displays.");
    } else {
        DebugLog("GetDisplaysForGPU(DXGI): Found " + std::to_string(displays.size()) + " displays (primary first).");
    }
    return displays;
}




