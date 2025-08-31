#pragma once
#include <vector>
#include <string>
#include "GPUInfo.h" 
#include <dxgi.h> // 追加

struct DisplayInfo {
    std::string name;
    std::string serialNumber;
    bool isPrimary = false;
    int portIndex = -1; // Add portIndex for sorting

};

class DisplayManager {
public:
    static std::vector<GPUInfo> GetInstalledGPUs(); // 追加
    static std::vector<DisplayInfo> GetDisplaysForGPU(const std::string& gpuVendorID, const std::string& gpuDeviceID);
    static bool CheckHardwareEncodingSupport(IDXGIAdapter* pAdapter);
};


