#include <windows.h>
#include "TaskTrayApp.h"
#include "DebugLog.h" // 追加

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

    TaskTrayApp app(hInstance);
    if (!app.Initialize()) {
        DebugLog("WinMain: Failed to initialize TaskTrayApp.");
        return -1;
    }
    DebugLog("WinMain: TaskTrayApp initialized successfully.");
    int rc = app.Run();
    // どの終了経路でも thread join / handle close を保証
    app.Cleanup();
    return rc;
}
