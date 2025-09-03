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
    void SelectDisplay(int displayIndex);
    void MonitorDisplayChanges();
    void RefreshDisplayList();
    bool Cleanup();


private:
    void UpdateTrayTooltip(const std::wstring& text);

    HINSTANCE hInstance;
    HWND hwnd;
    NOTIFYICONDATA nid;
    std::thread monitorThread;
    std::atomic<bool> running = true;
    std::string lastSystemPrimarySerial; // Tracks last OS primary monitor serial
};

#endif // TASKTRAYAPP_H

