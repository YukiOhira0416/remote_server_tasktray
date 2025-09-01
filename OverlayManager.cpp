#include "OverlayManager.h"
#include "DebugLog.h"
#include "SharedMemoryHelper.h"
#include "StringConversion.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <vector>
#include <dxgi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")

// DPI-aware dimensions
const int OVERLAY_WIDTH_DP = 220;
const int OVERLAY_HEIGHT_DP = 160;
const int FONT_SIZE_DP = 128;

OverlayManager& OverlayManager::Instance() {
    static OverlayManager instance;
    return instance;
}

void OverlayManager::Initialize(HINSTANCE hInstance, HWND owner) {
    hInst_ = hInstance;
    owner_ = owner;
    if (!classRegistered_) {
        RegisterWindowClass();
    }
}

void OverlayManager::RegisterWindowClass() {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = CLASS_NAME;
    wc.style = CS_HREDRAW | CS_VREDRAW;

    if (RegisterClassExW(&wc)) {
        classRegistered_ = true;
        DebugLog("OverlayManager: Window class registered successfully.");
    } else {
        DebugLog("OverlayManager: Failed to register window class.");
    }
}

LRESULT CALLBACK OverlayManager::OverlayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void OverlayManager::ShowNumberForSerial(int number, const std::string& serialUtf8) {
    DebugLog("OverlayManager: ShowNumberForSerial called for number " + std::to_string(number) + " on serial " + serialUtf8);
    HideAll(); // Hide any previously shown overlay

    RECT monRect;
    HMONITOR hMon;
    UINT dpi;
    if (ResolveMonitorInfoBySerial(serialUtf8, monRect, hMon, &dpi)) {
        // We can just use one window and move it around.
        if (overlayWindows_.empty()) {
            overlayWindows_.resize(1);
        }

        EnsureOverlayWindow(0, monRect, number, dpi);
        activeOverlayIndex_ = 0;
    } else {
        DebugLog("OverlayManager: Could not resolve monitor for serial: " + serialUtf8);
    }
}

void OverlayManager::HideAll() {
    if (activeOverlayIndex_ != -1) {
        DebugLog("OverlayManager: Hiding active overlay.");
        if (overlayWindows_[activeOverlayIndex_].hwnd) {
            ShowWindow(overlayWindows_[activeOverlayIndex_].hwnd, SW_HIDE);
        }
        activeOverlayIndex_ = -1;
    }
}

void OverlayManager::Cleanup() {
    DebugLog("OverlayManager: Cleanup called.");
    for (auto& window : overlayWindows_) {
        if (window.hwnd) {
            DestroyWindow(window.hwnd);
        }
    }
    overlayWindows_.clear();
}

bool OverlayManager::ResolveMonitorInfoBySerial(const std::string& serialUtf8, RECT& outRect, HMONITOR& outMon, UINT& outDpi) {
    DebugLog("OverlayManager: Resolving monitor info for serial: " + serialUtf8);

    SharedMemoryHelper smh(nullptr);
    std::string gpuInfo = smh.ReadSharedMemory("GPU_INFO");
    if (gpuInfo.empty()) {
        DebugLog("OverlayManager: GPU_INFO is empty. Cannot resolve monitor.");
        return false;
    }

    std::string gpuVendorID, gpuDeviceID;
    size_t delimiter = gpuInfo.find(":");
    if (delimiter != std::string::npos) {
        gpuVendorID = gpuInfo.substr(0, delimiter);
        gpuDeviceID = gpuInfo.substr(delimiter + 1);
    } else {
        DebugLog("OverlayManager: Invalid GPU_INFO format.");
        return false;
    }

    unsigned int targetVendorID = std::stoul(gpuVendorID);
    unsigned int targetDeviceID = std::stoul(gpuDeviceID);

    IDXGIFactory* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        DebugLog("OverlayManager: Failed to create DXGIFactory.");
        return false;
    }

    IDXGIAdapter* pAdapter = nullptr;
    bool foundAdapter = false;
    for (UINT i = 0; pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc;
        pAdapter->GetDesc(&desc);
        if (desc.VendorId == targetVendorID && desc.DeviceId == targetDeviceID) {
            foundAdapter = true;
            break; // Found the adapter
        }
        pAdapter->Release();
    }

    if (!foundAdapter) {
        DebugLog("OverlayManager: Could not find matching GPU adapter.");
        pFactory->Release();
        return false;
    }

    IDXGIOutput* pOutput = nullptr;
    for (UINT i = 0; pAdapter->EnumOutputs(i, &pOutput) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC outputDesc;
        pOutput->GetDesc(&outputDesc);

        MONITORINFOEXW mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(outputDesc.Monitor, &mi)) {
            DISPLAY_DEVICEW ddMonitor = { sizeof(ddMonitor) };
            if (EnumDisplayDevicesW(mi.szDevice, 0, &ddMonitor, EDD_GET_DEVICE_INTERFACE_NAME)) {
                std::string currentSerial = ConvertWStringToString(ddMonitor.DeviceID);
                if (currentSerial == serialUtf8) {
                    DebugLog("OverlayManager: Found matching monitor. Serial: " + currentSerial);
                    outRect = mi.rcMonitor;
                    outMon = outputDesc.Monitor;
                    UINT dpiX, dpiY;
                    if (SUCCEEDED(GetDpiForMonitor(outMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
                        outDpi = dpiX;
                    } else {
                        outDpi = 96; // Default DPI
                    }
                    pOutput->Release();
                    pAdapter->Release();
                    pFactory->Release();
                    return true;
                }
            }
        }
        pOutput->Release();
    }

    pAdapter->Release();
    pFactory->Release();
    DebugLog("OverlayManager: Failed to find a monitor with serial: " + serialUtf8);
    return false;
}

void OverlayManager::EnsureOverlayWindow(int index, const RECT& monRect, int number, UINT dpi) {
    OverlayWindow& overlay = overlayWindows_[index];
    overlay.currentNumber = number;

    if (!overlay.hwnd) {
        DebugLog("OverlayManager: Creating new overlay window.");
        // Window style for a non-interactive, topmost, transparent overlay
        DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        DWORD style = WS_POPUP;

        overlay.hwnd = CreateWindowExW(
            exStyle, CLASS_NAME, L"", style,
            0, 0, 0, 0, // Initial position and size, will be set later
            owner_, NULL, hInst_, NULL);

        if (!overlay.hwnd) {
            DebugLog("OverlayManager: Failed to create overlay window.");
            return;
        }
    }

    // Calculate DPI-scaled size and position
    int width = MulDiv(OVERLAY_WIDTH_DP, dpi, 96);
    int height = MulDiv(OVERLAY_HEIGHT_DP, dpi, 96);
    int x = monRect.right - width; // Position at bottom-right
    int y = monRect.bottom - height;

    // Create drawing resources
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // Draw the content
    DrawOverlayContent(hdcMem, {0, 0, width, height}, number, dpi);

    // Use UpdateLayeredWindow to set content and transparency
    BLENDFUNCTION blend = {0};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 200; // Semi-transparent (0-255)
    blend.AlphaFormat = AC_SRC_ALPHA;

    POINT ptSrc = {0, 0};
    SIZE sizeWnd = {width, height};
    POINT ptDst = {x, y};

    if (!UpdateLayeredWindow(overlay.hwnd, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA)) {
        DebugLog("OverlayManager: UpdateLayeredWindow failed. Error: " + std::to_string(GetLastError()));
    }

    // Cleanup GDI resources
    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    // Show the window without activating it
    ShowWindow(overlay.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(overlay.hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void OverlayManager::DrawOverlayContent(HDC hdc, const RECT& clientRect, int number, UINT dpi) {
    // Background (semi-transparent black rounded rectangle)
    HBRUSH bgBrush = CreateSolidBrush(RGB(10, 10, 10));
    SelectObject(hdc, bgBrush);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, clientRect.left, clientRect.top, clientRect.right, clientRect.bottom, 20, 20);
    DeleteObject(bgBrush);

    // Text (large number)
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);

    // Create a DPI-scaled font
    int fontSize = MulDiv(FONT_SIZE_DP, dpi, 96);
    HFONT hFont = CreateFontW(
        -fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");

    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    // Draw the number centered in the rectangle
    std::wstring text = std::to_wstring(number);
    DrawTextW(hdc, text.c_str(), -1, (LPRECT)&clientRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Cleanup font
    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}
