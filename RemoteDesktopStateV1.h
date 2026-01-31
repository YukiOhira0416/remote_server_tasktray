#pragma once
#include <cstdint>

// Definition matching the service side structure
struct RemoteDesktopStateV1 {
    uint32_t magic;         // 0x31534452 "RDS1"
    uint32_t version;       // 1
    wchar_t desktopName[256];
};
