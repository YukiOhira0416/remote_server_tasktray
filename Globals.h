#pragma once
#include <vector>

// グローバル変数の宣言

#define SHAREDMEMORY_NAME_GPU L"GPU_INFO"
#define SHAREDMEMORY_NAME_DISP L"DISP_INFO"
#define REG_PATH_GPU L"SOFTWARE\\MyApp\\GPUInfo"
#define REG_PATH_DISP L"SOFTWARE\\MyApp\\DISPInfo"
#define REG_PATH_CAPTURE_TYPE L"SOFTWARE\\MyApp\\CaptureType"

extern int serialNumberIndex;