#pragma once
#include <vector>
#include <string>
#include "GPUInfo.h"
#include <dxgi.h>
#include <d3d11.h> // 追加

class GPUManager {
public:
    static std::vector<GPUInfo> GetInstalledGPUs();
    static bool IsHardwareEncodingSupported();
private:
    static bool IsHardwareEncodingSupported(IDXGIAdapter* pAdapter); // 追加
};




