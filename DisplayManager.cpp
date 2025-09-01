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


// Helper function to enumerate display outputs for a given adapter in port order.
// This provides a deterministic order based on the physical connection sequence.
static bool EnumerateOutputsPortOrder(IDXGIAdapter* pAdapter, std::vector<DisplayInfo>& outDisplays) {
    IDXGIOutput* pOutput = nullptr;
    for (UINT i = 0; pAdapter->EnumOutputs(i, &pOutput) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC desc;
        if (FAILED(pOutput->GetDesc(&desc))) {
            DebugLog("EnumerateOutputsPortOrder: Failed to get output description.");
            pOutput->Release();
            continue;
        }

        MONITORINFOEXW mi;
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(desc.Monitor, &mi)) {
            DebugLog("EnumerateOutputsPortOrder: Failed to get monitor info.");
            pOutput->Release();
            continue;
        }

        DISPLAY_DEVICEW ddMonitor = { sizeof(ddMonitor) };
        ddMonitor.cb = sizeof(ddMonitor);
        // Use the device name from MONITORINFOEX to get the monitor's device details.
        if (!EnumDisplayDevicesW(mi.szDevice, 0, &ddMonitor, 0)) {
            DebugLog("EnumerateOutputsPortOrder: Failed to enumerate display devices for monitor.");
            pOutput->Release();
            continue;
        }

        DisplayInfo di;
        // Use DeviceID as the unique serial number, as per requirements.
        di.serialNumber = ConvertWStringToString(ddMonitor.DeviceID);
        // Use the more user-friendly DeviceString for the name.
        di.name = ConvertWStringToString(ddMonitor.DeviceString);
        di.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        outDisplays.push_back(di);
        DebugLog("EnumerateOutputsPortOrder: Found display - Name: " + di.name + ", Serial: " + di.serialNumber + ", Primary: " + std::to_string(di.isPrimary));

        pOutput->Release();
    }
    return !outDisplays.empty();
}

std::vector<DisplayInfo> DisplayManager::GetDisplaysForGPU(const std::string& gpuVendorID, const std::string& gpuDeviceID) {
    std::vector<DisplayInfo> displays;
    DebugLog("GetDisplaysForGPU: Start (DXGI) - VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);

    IDXGIFactory* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        DebugLog("GetDisplaysForGPU: Failed to create DXGIFactory.");
        return displays;
    }

    unsigned int targetVendorID = 0;
    unsigned int targetDeviceID = 0;
    try {
        targetVendorID = std::stoul(gpuVendorID);
        targetDeviceID = std::stoul(gpuDeviceID);
    }
    catch (const std::exception& e) {
        DebugLog("GetDisplaysForGPU: Failed to convert GPU IDs to integers: " + std::string(e.what()));
        pFactory->Release();
        return displays;
    }

    IDXGIAdapter* pAdapter = nullptr;
    for (UINT i = 0; pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc;
        if (FAILED(pAdapter->GetDesc(&desc))) {
            pAdapter->Release();
            continue;
        }

        if (desc.VendorId == targetVendorID && desc.DeviceId == targetDeviceID) {
            DebugLog("GetDisplaysForGPU: Found matching adapter. Enumerating outputs.");
            EnumerateOutputsPortOrder(pAdapter, displays);
            pAdapter->Release();
            break; // Found the target adapter, no need to continue.
        }

        pAdapter->Release();
    }

    pFactory->Release();

    if (displays.empty()) {
        DebugLog("GetDisplaysForGPU: No displays found for GPU VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    }

    return displays;
}

// Callback function for EnumDisplayMonitors to find the primary monitor
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMonitor, &mi)) {
        if (mi.dwFlags & MONITORINFOF_PRIMARY) {
            // This is the primary monitor. Store its serial number.
            std::string* primarySerial = reinterpret_cast<std::string*>(dwData);
            DISPLAY_DEVICEW ddMonitor = { sizeof(ddMonitor) };
            ddMonitor.cb = sizeof(ddMonitor);
            if (EnumDisplayDevicesW(mi.szDevice, 0, &ddMonitor, 0)) {
                *primarySerial = ConvertWStringToString(ddMonitor.DeviceID);
                return FALSE; // Stop enumeration since we found the primary
            }
        }
    }
    return TRUE; // Continue enumeration
}

std::string DisplayManager::GetSystemPrimaryDisplaySerial() {
    std::string primarySerial = "";
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&primarySerial));
    DebugLog("GetSystemPrimaryDisplaySerial: Found system primary display serial: " + (primarySerial.empty() ? "None" : primarySerial));
    return primarySerial;
}



