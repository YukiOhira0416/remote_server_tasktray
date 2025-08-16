#pragma once
#include <string>

struct GPUInfo {
    std::string vendorID;
    std::string deviceID;
    std::string name;
    bool supportsHardwareEncoding = false;
};
