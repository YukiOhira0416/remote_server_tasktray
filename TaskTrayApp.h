#ifndef TASKTRAYAPP_H
#define TASKTRAYAPP_H

#include <windows.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include "Globals.h"

class DisplaySyncServer;
class ModeSyncServer;
class TaskTrayApp {
public:
    TaskTrayApp(HINSTANCE hInstance);
    bool Initialize();
    int Run();
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void CreateTrayIcon();
    void ShowContextMenu();
    void UpdateDisplayMenu(HMENU hMenu);
    void UpdateCaptureModeMenu(HMENU hMenu);
    void SelectDisplay(int displayIndex);
    void GetDisplayStateForSync(int& outDisplayCount, int& outActiveDisplayIndex);
    void SetCaptureMode(int mode);
    void UpdateOptimizedPlanFromUi(int plan);
    void UpdateOptimizedPlanFromNetwork(int plan);
    int  GetOptimizedPlanForSync() const;
    void ShowControlPanel();
    bool RefreshDisplayList();
    bool Cleanup();


private:
    void UpdateTrayTooltip(const std::wstring& text);
    void ApplyOptimizedPlanToUi(int plan);

    HINSTANCE hInstance;
    HWND hwnd;
    NOTIFYICONDATA nid;
    DisplaySyncServer* displaySyncServer;
    ModeSyncServer* modeSyncServer;
    std::atomic<int> optimizedPlan{ 1 };
    std::atomic<bool> running = true;
    std::atomic<bool> cleaned = false;
};

#endif // TASKTRAYAPP_H

