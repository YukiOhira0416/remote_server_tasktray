#include "GPUManager.h"
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h> // 追加
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include "Utility.h"
#include "DebugLog.h" // 追加

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib") // 追加
#pragma comment(lib, "dxva2.lib") // 追加

bool GPUManager::IsHardwareEncodingSupported(IDXGIAdapter* pAdapter) { // 修正
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        pAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        NULL,
        0,
        NULL,
        0,
        D3D11_SDK_VERSION,
        &pDevice,
        &featureLevel,
        &pContext
    );

    if (FAILED(hr)) {
        DebugLog("IsHardwareEncodingSupported: Failed to create D3D11 device.");
        return false;
    }

    ID3D11VideoDevice* pVideoDevice = nullptr;
    hr = pDevice->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&pVideoDevice);
    pDevice->Release();
    pContext->Release();

    if (FAILED(hr)) {
        DebugLog("IsHardwareEncodingSupported: Failed to query ID3D11VideoDevice interface.");
        return false;
    }

    pVideoDevice->Release();
    DebugLog("IsHardwareEncodingSupported: Hardware encoding is supported.");
    return true;
}

std::vector<GPUInfo> GPUManager::GetInstalledGPUs() {
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
        gpu.supportsHardwareEncoding = IsHardwareEncodingSupported(pAdapter);

        gpus.push_back(gpu);
        pAdapter->Release();
    }
    pFactory->Release();
    DebugLog("GetInstalledGPUs: Retrieved installed GPUs.");
    return gpus;
}

bool GPUManager::IsHardwareEncodingSupported() {
    auto gpus = GetInstalledGPUs();
    for (const auto& gpu : gpus) {
        if (!gpu.supportsHardwareEncoding) {
            DebugLog("IsHardwareEncodingSupported: GPU does not support hardware encoding - VendorID: " + gpu.vendorID + ", DeviceID: " + gpu.deviceID);
            return false;
        }
    }
    DebugLog("IsHardwareEncodingSupported: All GPUs support hardware encoding.");
    return true;
}



