#include <windows.h>
// Additions for primary-adapter detection
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#include "TaskTrayApp.h"
#include "GPUManager.h"
#include "RegistryHelper.h"
#include "SharedMemoryHelper.h"
#include <functional>
#include <string>
#include <algorithm>
#include "GPUInfo.h"
#include "DebugLog.h" // 追加

// Finds the DXGI adapter (vendor/device) that owns the OS primary monitor.
// Returns true on success and fills vendorID/deviceID (decimal strings).
static bool GetPrimaryAdapterVendorDevice(std::string& outVendorID, std::string& outDeviceID) {
    IDXGIFactory* pFactory = nullptr;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        DebugLog("GetPrimaryAdapterVendorDevice: CreateDXGIFactory failed.");
        return false;
    }

    bool found = false;
    IDXGIAdapter* pAdapter = nullptr;

    for (UINT ai = 0; pFactory->EnumAdapters(ai, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        DXGI_ADAPTER_DESC ad;
        if (FAILED(pAdapter->GetDesc(&ad))) {
            pAdapter->Release();
            continue;
        }

        // Enumerate outputs in port order and look for the primary monitor flag.
        IDXGIOutput* pOutput = nullptr;
        for (UINT oi = 0; pAdapter->EnumOutputs(oi, &pOutput) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(pOutput->GetDesc(&od))) {
                MONITORINFOEXW mi;
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(od.Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
                    outVendorID = std::to_string(ad.VendorId);
                    outDeviceID = std::to_string(ad.DeviceId);
                    found = true;
                    pOutput->Release();
                    break;
                }
            }
            pOutput->Release();
        }

        pAdapter->Release();
        if (found) break;
    }

    pFactory->Release();
    if (!found) {
        DebugLog("GetPrimaryAdapterVendorDevice: OS primary adapter not found (falling back).");
    } else {
        DebugLog("GetPrimaryAdapterVendorDevice: Found OS primary adapter.");
    }
    return found;
}

int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    // Set Per-Monitor DPI Awareness to handle different DPIs across monitors.
    // This is crucial for ensuring the tray menu and tooltips are positioned correctly.
    HMODULE user32 = LoadLibrary(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI* SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
        auto setProcessDpiAwarenessContext = (SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setProcessDpiAwarenessContext) {
            // Try for Per-Monitor V2 awareness.
            if (setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
                DebugLog("WinMain: Set DPI awareness to Per Monitor Aware V2.");
            } else {
                DebugLog("WinMain: Failed to set DPI awareness to Per Monitor Aware V2.");
            }
        } else {
            // Fallback for older systems.
            if (SetProcessDPIAware()) {
                DebugLog("WinMain: Set DPI awareness to System DPI Aware as a fallback.");
            } else {
                DebugLog("WinMain: Failed to set DPI awareness.");
            }
        }
        FreeLibrary(user32);
    }

    // 搭載されているGPUを取得
    auto gpus = GPUManager::GetInstalledGPUs();
    int gpuCount = static_cast<int>(gpus.size()); // 適切にキャスト
    DebugLog("WinMain: Number of GPUs installed: " + std::to_string(gpuCount));

    if (!GPUManager::IsHardwareEncodingSupported()) {
        MessageBoxW(NULL, L"ハードウェアエンコードに未対応のGPUがあるためアプリケーションを終了します", L"エラー", MB_OK | MB_ICONERROR);
        DebugLog("WinMain: Hardware encoding not supported. Exiting application.");
        return -1;
    }

    // レジストリと共有メモリの情報を比較
    auto registry = RegistryHelper::ReadRegistry();
    DebugLog("WinMain: Read registry - VendorID: " + registry.first + ", DeviceID: " + registry.second);
    // ------------------------------------------------------------
    // Determine which GPU owns the OS primary display at startup.
    // If found, prefer that adapter; otherwise fall back to existing logic.
    // ------------------------------------------------------------
    GPUInfo onlyGPU;
    std::string primaryVendorID, primaryDeviceID;
    bool hasPrimaryAdapter = GetPrimaryAdapterVendorDevice(primaryVendorID, primaryDeviceID); // uses DXGI

    if (hasPrimaryAdapter) {
        // Prefer the adapter that owns the OS primary display
        onlyGPU.vendorID = primaryVendorID;
        onlyGPU.deviceID = primaryDeviceID;
        // Optional: name can remain empty or you may fill it by scanning GPUManager::GetInstalledGPUs if needed.
    } else {
        // Fallback to previous behavior: pick the single/last enumerated GPU
        // **レジストリと共有メモリの両方にgpuinfoが存在しない場合**
        auto gpus = GPUManager::GetInstalledGPUs();
        for (const auto& gpu : gpus) {
            onlyGPU = gpu;
        }
    }

    SharedMemoryHelper sharedMemoryHelper(nullptr); // インスタンスを作成

    if (registry.first.empty()) {
        if (RegistryHelper::WriteRegistry(onlyGPU.vendorID, onlyGPU.deviceID)) {
            DebugLog("WinMain: Wrote GPU info to registry - VendorID: " + onlyGPU.vendorID + ", DeviceID: " + onlyGPU.deviceID);
        }
        else {
            DebugLog("WinMain: Failed to write GPU info to registry.");
        }
        if (sharedMemoryHelper.WriteSharedMemory("GPU_INFO", onlyGPU.vendorID + ":" + onlyGPU.deviceID)) {
            DebugLog("WinMain: Wrote GPU info to shared memory - " + onlyGPU.vendorID + ":" + onlyGPU.deviceID);
        }
        else {
            DebugLog("WinMain: Failed to write GPU info to shared memory.");
        }
    }
    else {
        if (onlyGPU.vendorID != registry.first || onlyGPU.deviceID != registry.second) {
            // **レジストリの情報と異なる場合
            if (RegistryHelper::WriteRegistry(onlyGPU.vendorID, onlyGPU.deviceID)) {
                DebugLog("WinMain: Updated registry with new GPU info - VendorID: " + onlyGPU.vendorID + ", DeviceID: " + onlyGPU.deviceID);
            }
            else {
                DebugLog("WinMain: Failed to update registry with new GPU info.");
            }
            if (sharedMemoryHelper.WriteSharedMemory("GPU_INFO", onlyGPU.vendorID + ":" + onlyGPU.deviceID)) {
                DebugLog("WinMain: Updated shared memory with new GPU info - " + onlyGPU.vendorID + ":" + onlyGPU.deviceID);
            }
            else {
                DebugLog("WinMain: Failed to update shared memory with new GPU info.");
            }
        }
        else {
            // **一致する場合は `GPU_INFO` を更新**
            if (sharedMemoryHelper.WriteSharedMemory("GPU_INFO", registry.first + ":" + registry.second)) {
                DebugLog("WinMain: Updated shared memory with registry GPU info - " + registry.first + ":" + registry.second);
            }
            else {
                DebugLog("WinMain: Failed to update shared memory with registry GPU info.");
            }
        }
    }

    TaskTrayApp app(hInstance);
    if (!app.Initialize()) {
        DebugLog("WinMain: Failed to initialize TaskTrayApp.");
        return -1;
    }
    DebugLog("WinMain: TaskTrayApp initialized successfully.");
    return app.Run();
}
