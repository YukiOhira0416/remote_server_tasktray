#ifndef TASKTRAYAPP_H
#define TASKTRAYAPP_H

#include <windows.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include "Globals.h"

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
    void SetCaptureMode(int mode);
    void ShowControlPanel();
    void MonitorDisplayChanges();
    bool RefreshDisplayList();
    bool Cleanup();


private:
    void UpdateTrayTooltip(const std::wstring& text);
    void PulseRebootFlag();

    HINSTANCE hInstance;
    HWND hwnd;
    NOTIFYICONDATA nid;
    std::thread monitorThread;
    std::atomic<bool> running = true;
    std::atomic<bool> cleaned = false;
    std::string lastSystemPrimarySerial; // Tracks last OS primary monitor serial
};

#endif // TASKTRAYAPP_H

