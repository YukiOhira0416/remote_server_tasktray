#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <memory>

class OverlayManager {
public:
    static OverlayManager& Instance();

    // Delete copy constructor and assignment operator for singleton pattern
    OverlayManager(const OverlayManager&) = delete;
    OverlayManager& operator=(const OverlayManager&) = delete;

    void Initialize(HINSTANCE hInstance, HWND owner);
    void ShowNumberForSerial(int number, const std::string& serialUtf8);
    void HideAll();
    void Cleanup();

private:
    OverlayManager() = default;
    ~OverlayManager() = default;

    struct OverlayWindow {
        HWND hwnd = nullptr;
        int currentNumber = -1;
        std::string currentSerial;
    };

    // Private helper methods
    bool ResolveMonitorInfoBySerial(const std::string& serialUtf8, RECT& outRect, HMONITOR& outMon, UINT& outDpi);
    void EnsureOverlayWindow(int index, const RECT& monRect, int number, UINT dpi);
    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void RegisterWindowClass();
    void DrawOverlayContent(HDC hdc, const RECT& clientRect, int number, UINT dpi);

    // Member variables
    HINSTANCE hInst_ = nullptr;
    HWND owner_ = nullptr;
    std::vector<OverlayWindow> overlayWindows_;
    int activeOverlayIndex_ = -1;
    const wchar_t* CLASS_NAME = L"OverlayWindowClass";
    bool classRegistered_ = false;
};
