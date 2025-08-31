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
    DebugLog("GetDisplaysForGPU: Start - VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);

    DISPLAY_DEVICE dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);

    // Regex for VEN_/DEV_
    std::regex vendorRegex("VEN_([0-9A-Fa-f]+)");
    std::regex deviceRegex("DEV_([0-9A-Fa-f]+)");

    // Parse target GPU IDs (decimal form used in current codebase)
    unsigned int gpuVendorIDDec = 0, gpuDeviceIDDec = 0;
    try {
        gpuVendorIDDec = static_cast<unsigned int>(std::stoul(gpuVendorID));
        gpuDeviceIDDec = static_cast<unsigned int>(std::stoul(gpuDeviceID));
    } catch (const std::exception& e) {
        DebugLog(std::string("GetDisplaysForGPU: Failed to parse GPU IDs: ") + e.what());
        return displays;
    }

    struct Row {
        DisplayInfo di;
        int displayNumber; // from "\\.\DISPLAYn" -> n
    };
    std::vector<Row> rows;

    for (int deviceIndex = 0; EnumDisplayDevices(nullptr, deviceIndex, &dd, 0); ++deviceIndex) {
        if ((dd.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) {
            continue;
        }

        // Extract adapter VEN/DEV (from dd.DeviceID)
        std::string adapterDevID = ConvertWStringToString(dd.DeviceID);
        std::smatch venMatch, devMatch;
        std::string venHex, devHex;
        if (std::regex_search(adapterDevID, venMatch, vendorRegex) && venMatch.size() > 1) venHex = venMatch.str(1);
        if (std::regex_search(adapterDevID, devMatch, deviceRegex) && devMatch.size() > 1) devHex = devMatch.str(1);

        unsigned int venDec = 0, devDec = 0;
        try {
            venDec = std::stoul(venHex, nullptr, 16);
            devDec = std::stoul(devHex, nullptr, 16);
        } catch (...) {
            continue;
        }

        if (venDec != gpuVendorIDDec || devDec != gpuDeviceIDDec) {
            continue; // Not the GPU we care about
        }

        // For this adapter, enumerate its monitor (index 0)
        DISPLAY_DEVICE ddMonitor;
        ZeroMemory(&ddMonitor, sizeof(ddMonitor));
        ddMonitor.cb = sizeof(ddMonitor);
        if (!EnumDisplayDevices(dd.DeviceName, 0, &ddMonitor, 0)) {
            DebugLog("GetDisplaysForGPU: Failed to enumerate monitor for " + ConvertWStringToString(dd.DeviceName));
            continue;
        }

        // Build DisplayInfo
        DisplayInfo di;
        di.name = ConvertWStringToString(dd.DeviceName);       // e.g., "\\\\.\\DISPLAY1"
        di.serialNumber = ConvertWStringToString(ddMonitor.DeviceID); // e.g., "MONITOR\\DELA0.."
        di.isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

        // Extract numeric suffix from "\\.\DISPLAYn"
        int n = 0;
        {
            // dd.DeviceName is wide — convert to narrow then parse digits
            std::string nameNarrow = ConvertWStringToString(dd.DeviceName);
            // Find trailing number
            int acc = 0;
            for (size_t i = 0; i < nameNarrow.size(); ++i) {
                if (isdigit(static_cast<unsigned char>(nameNarrow[i]))) {
                    acc = acc * 10 + (nameNarrow[i] - '0');
                }
            }
            n = acc; // 0 if not found; DISPLAY1 -> 1, etc.
        }

        rows.push_back({ di, n });
        DebugLog("GetDisplaysForGPU: Found display - Name: " + di.name + ", SerialNumber: " + di.serialNumber + ", IsPrimary: " + std::to_string(di.isPrimary) + ", Num: " + std::to_string(n));
    }

    if (rows.empty()) {
        DebugLog("GetDisplaysForGPU: No displays found for GPU VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
        return displays;
    }

    // Sort: primary first (true > false), then by displayNumber ascending
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.di.isPrimary != b.di.isPrimary) return a.di.isPrimary > b.di.isPrimary;
        return a.displayNumber < b.displayNumber;
    });

    displays.reserve(rows.size());
    for (const auto& r : rows) {
        displays.push_back(r.di);
    }

    return displays;
}




