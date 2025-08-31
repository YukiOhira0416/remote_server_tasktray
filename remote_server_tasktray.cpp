#include <windows.h>
#include "TaskTrayApp.h"
#include "GPUManager.h"
#include "RegistryHelper.h"
#include "SharedMemoryHelper.h"
#include <functional>
#include <string>
#include <algorithm>
#include "GPUInfo.h"
#include "DebugLog.h" // 追加
#include <sstream>
#include <regex>
#include "StringConversion.h"

int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {

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
    GPUInfo onlyGPU;

    // **レジストリと共有メモリの両方にgpuinfoが存在しない場合**
    bool foundPrimaryAdapter = false;
    DISPLAY_DEVICE dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevices(NULL, i, &dd, 0); ++i) {
        if ((dd.StateFlags & DISPLAY_DEVICE_ACTIVE) && (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) {
            std::string deviceID = ConvertWStringToString(dd.DeviceID);
            std::smatch m1, m2;
            std::regex vendorRegex("VEN_([0-9A-Fa-f]+)");
            std::regex deviceRegex("DEV_([0-9A-Fa-f]+)");
            std::string venHex, devHex;
            if (std::regex_search(deviceID, m1, vendorRegex) && m1.size() > 1) venHex = m1.str(1);
            if (std::regex_search(deviceID, m2, deviceRegex) && m2.size() > 1) devHex = m2.str(1);
            unsigned venDec = 0, devDec = 0;
            std::stringstream ss;
            ss << std::hex << venHex; ss >> venDec; ss.clear();
            ss << std::hex << devHex; ss >> devDec;

            for (const auto& gpu : gpus) {
                if ((unsigned)std::stoi(gpu.vendorID) == venDec &&
                    (unsigned)std::stoi(gpu.deviceID) == devDec) {
                    onlyGPU = gpu;
                    foundPrimaryAdapter = true;
                    break;
                }
            }
            if (foundPrimaryAdapter) break;
        }
    }

    if (!foundPrimaryAdapter) {
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
