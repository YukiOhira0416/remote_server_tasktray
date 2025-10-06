#include "TaskTrayApp.h"
#include "StringConversion.h"
#include "Utility.h"
#include "GPUManager.h"
#include "DisplayManager.h"
#include "RegistryHelper.h"
#include "SharedMemoryHelper.h"
#include <tchar.h>
#include <iostream>
#include <algorithm>
#include <functional>
#include <windows.h>
#include <vector>
#include <string>
#include <CommCtrl.h>
#include "DebugLog.h"
#include "Globals.h"
#include "OverlayManager.h"
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <dbt.h>
#include <atomic>
#include <thread>
#include <memory>
#include <cstdint>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/Qt>
#include "ui_Main_UI.h"


namespace {
constexpr UINT ID_EXIT = 1;
constexpr UINT ID_DISPLAY_BASE = 100;
constexpr UINT ID_CAPTURE_MODE_NORMAL = 200;
constexpr UINT ID_CAPTURE_MODE_GAME = 201;
constexpr UINT ID_CONTROL_PANEL = 300;

std::atomic<bool> g_controlPanelRunning{ false };
std::atomic<QMainWindow*> g_controlPanelWindow{ nullptr };
std::atomic<std::uint64_t> g_controlPanelToken{ 0 };
}


// Register the TaskbarCreated message. This is sent when the taskbar is created (e.g., after an explorer.exe crash).
// We need to handle this to re-add our icon.
UINT WM_TASKBARCREATED = RegisterWindowMessage(_T("TaskbarCreated"));

std::filesystem::path GetExecutablePath() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

TaskTrayApp::TaskTrayApp(HINSTANCE hInst) : hInstance(hInst), hwnd(NULL), running(true) {
    ZeroMemory(&nid, sizeof(nid));
}


bool TaskTrayApp::Initialize() {
    // 実行ファイルのパスを取得
    std::filesystem::path exePath = GetExecutablePath();
    std::filesystem::path logFilePath = exePath / "debuglog_tasktray.log";

    // 既存のログファイルをバックアップする
    if (std::filesystem::exists(logFilePath)) {
        // 現在の日時を取得
        std::time_t t = std::time(nullptr);
        std::tm tm;
        localtime_s(&tm, &t);

        // 日付文字列を作成
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d%H%M%S");
        std::string timestamp = oss.str();

        // バックアップファイル名を作成
        std::string backupFileName = timestamp + "_debuglog_tasktray.log.back";
        std::filesystem::path backupFilePath = exePath / backupFileName;

        // ファイルをリネーム
        std::filesystem::rename(logFilePath, backupFilePath);

        // バックアップファイルの数を確認し、5つを超える場合は古いものから削除
        std::vector<std::filesystem::path> backupFiles;
        for (const auto& entry : std::filesystem::directory_iterator(exePath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                const std::string suffix = "_debuglog_tasktray.log.back";
            if (filename.length() >= suffix.length() &&
                filename.compare(filename.length() - suffix.length(), suffix.length(), suffix) == 0) {
                    backupFiles.push_back(entry.path());
                }
            }
        }

        // 日付順でソート（新しい順）
        std::sort(backupFiles.begin(), backupFiles.end(), std::greater<std::filesystem::path>());

        // 5つより多い場合、古いファイルを削除
        while (backupFiles.size() > 5) {
            std::filesystem::remove(backupFiles.back());
            backupFiles.pop_back();
        }
    }

    // ここから既存のコード
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = _T("TaskTrayClass");

    if (!RegisterClass(&wc)) {
        return false;
    }

    hwnd = CreateWindow(_T("TaskTrayClass"), _T("Task Tray App"), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, hInstance, this);

    if (!hwnd) return false;

    CreateTrayIcon();

    // 初回ディスプレイ情報取得
    RefreshDisplayList();

    // Initialize unplug-notification flag (REBOOT) to 0
    {
        SharedMemoryHelper sharedMemoryHelper(this);
        sharedMemoryHelper.WriteSharedMemory("REBOOT", "0");
        sharedMemoryHelper.WriteSharedMemory("Capture_Mode", "1");
    }

    // ディスプレイの接続・切断を監視するスレッドを起動
    monitorThread = std::thread(&TaskTrayApp::MonitorDisplayChanges, this);

    return true;
}


void TaskTrayApp::CreateTrayIcon() {
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 1;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpy(nid.szTip, _T("GPU & Display Manager"));

    Shell_NotifyIcon(NIM_ADD, &nid);
}

bool TaskTrayApp::Cleanup() {
    // Clean up overlay windows
    OverlayManager::Instance().Cleanup();

    // タスクトレイアイコンを削除
    Shell_NotifyIcon(NIM_DELETE, &nid);

    // スレッドの停止を指示
    running = false;
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    // 共有メモリとイベントを削除
    SharedMemoryHelper sharedMemoryHelper(this);
    sharedMemoryHelper.DeleteSharedMemory();
    sharedMemoryHelper.DeleteEvent();

    return true;
}

void TaskTrayApp::ShowContextMenu() {
    if (hwnd == nullptr) {
        DebugLog("ShowContextMenu: Error - hwnd is nullptr.");
        return;
    }

    POINT pt;
    if (!GetCursorPos(&pt)) {
        DebugLog("ShowContextMenu: Error - Failed to get cursor position.");
        return;
    }

    HMENU hMenu = CreatePopupMenu();
    if (hMenu == NULL) {
        DebugLog("ShowContextMenu: Error - Failed to create popup menu.");
        return;
    }

    // Add "Display Selection" submenu
    HMENU hSubMenu = CreatePopupMenu();
    if (hSubMenu == NULL) {
        DebugLog("ShowContextMenu: Error - Failed to create submenu.");
        DestroyMenu(hMenu);
        return;
    }

    UpdateDisplayMenu(hSubMenu); // Build the menu from shared memory
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, _T("Display Selection"));

    HMENU hCaptureMenu = CreatePopupMenu();
    if (hCaptureMenu == NULL) {
        DebugLog("ShowContextMenu: Error - Failed to create capture submenu.");
        DestroyMenu(hSubMenu);
        DestroyMenu(hMenu);
        return;
    }

    UpdateCaptureModeMenu(hCaptureMenu);
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hCaptureMenu, _T("CaptureMode"));

    AppendMenu(hMenu, MF_STRING, ID_CONTROL_PANEL, _T("ControlPanel"));

    // Add separator and Exit item
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_EXIT, _T("Exit")); // Command ID 1 for Exit

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu) {
    DebugLog("UpdateDisplayMenu: Start updating display menu from shared memory.");

    // Clear any existing menu items.
    while (GetMenuItemCount(hMenu) > 0) {
        RemoveMenu(hMenu, 0, MF_BYPOSITION);
    }

    SharedMemoryHelper sharedMemoryHelper(this);
    std::string numDisplaysStr = sharedMemoryHelper.ReadSharedMemory("DISP_INFO_NUM");
    int numDisplays = 0;
    if (!numDisplaysStr.empty()) {
        try {
            numDisplays = std::stoi(numDisplaysStr);
        }
        catch (const std::exception& e) {
            DebugLog("UpdateDisplayMenu: Failed to parse DISP_INFO_NUM: " + std::string(e.what()));
            AppendMenu(hMenu, MF_STRING | MF_GRAYED, 0, _T("Error reading displays"));
            return;
        }
    }

    if (numDisplays == 0) {
        AppendMenu(hMenu, MF_STRING | MF_GRAYED, 0, _T("No displays found"));
        return;
    }

    std::string selectedDisplaySerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    DebugLog("UpdateDisplayMenu: Currently selected display serial: " + selectedDisplaySerial);

    for (int idx = 0; idx < numDisplays; ++idx) {
        std::string key = "DISP_INFO_" + std::to_string(idx);
        std::string currentDisplaySerial = sharedMemoryHelper.ReadSharedMemory(key);

        UINT flags = MF_STRING;
        if (!currentDisplaySerial.empty() && currentDisplaySerial == selectedDisplaySerial) {
            flags |= MF_CHECKED;
        }

        std::wstring displayName = L"Display " + std::to_wstring(idx + 1);
        UINT commandId = ID_DISPLAY_BASE + idx; // Menu command IDs are 0-indexed.

        if (!AppendMenu(hMenu, flags, commandId, displayName.c_str())) {
            DebugLog("UpdateDisplayMenu: Failed to add menu item for Display " + std::to_string(idx + 1));
        }
    }

    DebugLog("UpdateDisplayMenu: Finished updating display menu.");
}

void TaskTrayApp::SelectDisplay(int displayIndex) {
    // displayIndex is 0-based from the menu and maps to DISP_INFO_{index}
    DebugLog("SelectDisplay: User selected display at index " + std::to_string(displayIndex));

    SharedMemoryHelper sharedMemoryHelper(this);

    // Read the serial number for the selected index from shared memory (0-based)
    std::string key = "DISP_INFO_" + std::to_string(displayIndex);
    std::string selectedSerial = sharedMemoryHelper.ReadSharedMemory(key);

    if (selectedSerial.empty()) {
        DebugLog("SelectDisplay: Could not find serial number for display index " + std::to_string(displayIndex) + " with key " + key);
        return;
    }

    // Persist the new selection to shared memory and registry
    sharedMemoryHelper.WriteSharedMemory("DISP_INFO", selectedSerial);
    RegistryHelper::WriteSelectedSerialToRegistry(selectedSerial);

    DebugLog("SelectDisplay: New display selected. Serial: " + selectedSerial);

    // Update the tray icon tooltip to reflect the new selection
    std::wstring newTooltip = L"Display Manager - Selected: Display " + std::to_wstring(displayIndex + 1);
    UpdateTrayTooltip(newTooltip);

    // The menu check mark will be updated the next time it's opened,
    // as UpdateDisplayMenu reads the latest selection from DISP_INFO.
}

void TaskTrayApp::SetCaptureMode(int mode) {
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string modeValue = std::to_string(mode);
    DebugLog("SetCaptureMode: Setting capture mode to " + modeValue);
    sharedMemoryHelper.WriteSharedMemory("Capture_Mode", modeValue);
    PulseRebootFlag();
}

void TaskTrayApp::ShowControlPanel() {
    if (auto window = g_controlPanelWindow.load()) {
        QMetaObject::invokeMethod(window, [window]() {
            window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
            window->show();
            window->raise();
            window->activateWindow();
        }, Qt::QueuedConnection);
        DebugLog("ShowControlPanel: Control panel already running. Bringing window to front.");
        return;
    }

    bool expected = false;
    if (!g_controlPanelRunning.compare_exchange_strong(expected, true)) {
        DebugLog("ShowControlPanel: Control panel launch already in progress. No new window created.");
        return;
    }

    std::uint64_t token = g_controlPanelToken.fetch_add(1) + 1;

    DebugLog("ShowControlPanel: Launching control panel UI.");

    std::thread([token]() {
        int argc = 0;
        char* argv[] = { nullptr };
        QApplication app(argc, argv);

        auto mainWindow = std::make_unique<QMainWindow>();
        Ui_MainWindow ui;
        ui.setupUi(mainWindow.get());

        QMainWindow* rawWindow = mainWindow.get();

        auto resetState = [token, rawWindow]() {
            bool pointerCleared = false;
            if (g_controlPanelWindow.load() == rawWindow) {
                g_controlPanelWindow.store(nullptr);
                pointerCleared = true;
                DebugLog("ShowControlPanel: Cleared control panel window pointer.");
            }

            if (g_controlPanelToken.load() == token) {
                if (g_controlPanelRunning.exchange(false)) {
                    DebugLog("ShowControlPanel: Cleared control panel running flag.");
                }
            }
            else if (pointerCleared) {
                DebugLog("ShowControlPanel: Window pointer cleared for stale control panel session.");
            }
        };

        struct CleanupGuard {
            std::function<void()> fn;
            ~CleanupGuard() { if (fn) fn(); }
        } cleanupGuard{ resetState };

        DebugLog("ShowControlPanel: Control panel window initialized.");

        g_controlPanelWindow.store(rawWindow);

        QObject::connect(&app, &QApplication::aboutToQuit, [resetState]() {
            DebugLog("ShowControlPanel: QApplication about to quit.");
            resetState();
        });

        QObject::connect(rawWindow, &QObject::destroyed, [resetState]() {
            DebugLog("ShowControlPanel: Control panel window destroyed.");
            resetState();
        });

        rawWindow->show();
        DebugLog("ShowControlPanel: Entering Qt event loop for control panel.");
        app.exec();
        DebugLog("ShowControlPanel: Qt event loop exited.");
    }).detach();
}

void TaskTrayApp::PulseRebootFlag() {
    std::thread([this]() {
        SharedMemoryHelper helper(this);
        DebugLog("PulseRebootFlag: Setting REBOOT to 1.");
        helper.WriteSharedMemory("REBOOT", "1");
        Sleep(1000);
        DebugLog("PulseRebootFlag: Resetting REBOOT to 0.");
        helper.WriteSharedMemory("REBOOT", "0");
    }).detach();
}

LRESULT CALLBACK TaskTrayApp::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    TaskTrayApp* app = nullptr;
    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        app = reinterpret_cast<TaskTrayApp*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        // Initialize the OverlayManager singleton
        OverlayManager::Instance().Initialize(app->hInstance, hwnd);
    }
    else {
        app = reinterpret_cast<TaskTrayApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (app) {
        if (uMsg == WM_TASKBARCREATED) {
            DebugLog("WindowProc: TaskbarCreated message received. Re-creating icon.");
            app->CreateTrayIcon();
            // Fall-through to WM_USER+2 to also update the tooltip
        }

        switch (uMsg) {
        case WM_USER + 1: // Our custom message for tray icon events
            if (lParam == WM_RBUTTONUP) {
                DebugLog("WindowProc: Tray icon right-clicked.");
                app->ShowContextMenu();
            }
            break;

        case WM_USER + 2: // Custom message to refresh UI (e.g., after display change)
        {
            DebugLog("WindowProc: WM_USER + 2 - Refreshing UI.");
            SharedMemoryHelper sharedMemoryHelper(app);
            std::string selectedSerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
            std::string numDisplaysStr = sharedMemoryHelper.ReadSharedMemory("DISP_INFO_NUM");
            int numDisplays = 0;
            if (!numDisplaysStr.empty()) {
                try { numDisplays = std::stoi(numDisplaysStr); }
                catch (...) {}
            }

            int selectedIndex = -1;
            for (int idx = 0; idx < numDisplays; ++idx) {
                std::string key = "DISP_INFO_" + std::to_string(idx);
                if (sharedMemoryHelper.ReadSharedMemory(key) == selectedSerial) {
                    selectedIndex = idx + 1; // human-friendly 1-based label
                    break;
                }
            }

            if (selectedIndex != -1) {
                app->UpdateTrayTooltip(L"Display Manager - Selected: Display " + std::to_wstring(selectedIndex));
            }
            else if (numDisplays > 0) {
                app->UpdateTrayTooltip(L"Display Manager");
            }
            else {
                app->UpdateTrayTooltip(L"Display Manager - No displays");
            }
        }
        break;

        case WM_DISPLAYCHANGE:
            DebugLog("WindowProc: WM_DISPLAYCHANGE received. Posting UI refresh message.");
            // Hide any active overlays in case the menu is open during a display change.
            OverlayManager::Instance().HideAll();
            // Display configuration changed. Post a message to ourselves to update the UI.
            // The background thread will handle the logic, this just updates the tooltip.
            PostMessage(app->hwnd, WM_USER + 2, 0, 0);
            break;

        case WM_MENUSELECT:
        {
            UINT cmdId = LOWORD(wParam);
            UINT flags = HIWORD(wParam);

            if ((flags & MF_HILITE) && !(flags & MF_POPUP)) {
                if (cmdId >= ID_DISPLAY_BASE && cmdId < 200) { // Display items are in this range
                    int idx0 = (cmdId - ID_DISPLAY_BASE);      // 0-based index for shared memory
                    int overlayNumber = idx0 + 1;  // 1-based number for display label
                    SharedMemoryHelper smh(app);
                    std::string key = "DISP_INFO_" + std::to_string(idx0);
                    std::string serial = smh.ReadSharedMemory(key);
                    if (!serial.empty()) {
                        OverlayManager::Instance().ShowNumberForSerial(overlayNumber, serial);
                    } else {
                        OverlayManager::Instance().HideAll();
                    }
                } else {
                    // Not a display item, hide the overlay
                    OverlayManager::Instance().HideAll();
                }
            }
            break;
        }

        case WM_EXITMENULOOP:
        case WM_UNINITMENUPOPUP:
            // Hide overlay when the menu is closed for any reason
            OverlayManager::Instance().HideAll();
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_EXIT) { // Exit command
                DebugLog("WindowProc: Exit command received.");
                if (app->Cleanup()) {
                    DebugLog("WindowProc: Cleanup succeeded.");
                    PostQuitMessage(0);
                }
                else {
                    DebugLog("WindowProc: Cleanup failed.");
                }
            }
            else if (LOWORD(wParam) >= ID_DISPLAY_BASE && LOWORD(wParam) < 200) { // Display selection
                DebugLog("WindowProc: Display selection command received.");
                app->SelectDisplay(LOWORD(wParam) - ID_DISPLAY_BASE);
            }
            else if (LOWORD(wParam) == ID_CAPTURE_MODE_NORMAL) {
                DebugLog("WindowProc: Normal Mode selected.");
                app->SetCaptureMode(1);
            }
            else if (LOWORD(wParam) == ID_CAPTURE_MODE_GAME) {
                DebugLog("WindowProc: Game Mode selected.");
                app->SetCaptureMode(2);
            }
            else if (LOWORD(wParam) == ID_CONTROL_PANEL) {
                DebugLog("WindowProc: Control Panel selected.");
                app->ShowControlPanel();
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        default:
            if (uMsg != WM_TASKBARCREATED) {
                 return DefWindowProc(hwnd, uMsg, wParam, lParam);
            }
            break;
        }
    } else {
         return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}


void TaskTrayApp::MonitorDisplayChanges() {
    while (running) {
        Sleep(2000); // Poll every 2 seconds

        // Get GPU info for enumeration
        SharedMemoryHelper sharedMemoryHelper(this);
        std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
        if (gpuInfo.empty()) {
            DebugLog("MonitorDisplayChanges: GPU_INFO is empty. Skipping check.");
            continue;
        }

        std::string gpuVendorID, gpuDeviceID;
        size_t delimiter = gpuInfo.find(":");
        if (delimiter != std::string::npos) {
            gpuVendorID = gpuInfo.substr(0, delimiter);
            gpuDeviceID = gpuInfo.substr(delimiter + 1);
        }
        else {
            DebugLog("MonitorDisplayChanges: Invalid GPU_INFO format.");
            continue;
        }

        // Get current OS primary monitor serial and detect change
        std::string currentSystemPrimary = DisplayManager::GetSystemPrimaryDisplaySerial();
        bool primaryChanged = false;
        if (!currentSystemPrimary.empty() && currentSystemPrimary != lastSystemPrimarySerial) {
            primaryChanged = true;
            lastSystemPrimarySerial = currentSystemPrimary;
        }

        // Get current display list from hardware
        auto newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);

        // Get old display list from registry
        std::vector<std::string> oldDisplaySerials = RegistryHelper::ReadDISPInfoFromRegistry();

        // Convert new display list to a vector of serials for easy comparison
        std::vector<std::string> newDisplaySerials;
        for (const auto& display : newDisplays) {
            newDisplaySerials.push_back(display.serialNumber);
        }

        // Only proceed if the display topology changed, or OS primary changed
        if (oldDisplaySerials == newDisplaySerials && !primaryChanged) {
            continue;
        }

        DebugLog("MonitorDisplayChanges: Detected topology or OS primary change.");

        // Detect if any known display disappeared (unplugged)
        bool removalDetected = false;
        for (const auto& oldSerial : oldDisplaySerials) {
            if (std::find(newDisplaySerials.begin(), newDisplaySerials.end(), oldSerial) == newDisplaySerials.end()) {
                removalDetected = true;
                break;
            }
        }

        // Detect if any new display appeared (plugged in)
        bool additionDetected = false;
        for (const auto& newSerial : newDisplaySerials) {
            if (std::find(oldDisplaySerials.begin(), oldDisplaySerials.end(), newSerial) == oldDisplaySerials.end()) {
                additionDetected = true;
                break;
            }
        }

        // Get the last known selected display from the registry
        std::string lastSelectedSerial = RegistryHelper::ReadSelectedSerialFromRegistry();
        std::string newSelectedSerial = "";

        // Check if the previously selected display is still connected
        bool selectionStillExists = false;
        if (!lastSelectedSerial.empty()) {
            for (const auto& display : newDisplays) {
                if (display.serialNumber == lastSelectedSerial) {
                    selectionStillExists = true;
                    break;
                }
            }
        }

        // Decide newSelectedSerial. The indexed list remains strictly in port order.
        if (primaryChanged) {
            // Force selection to the new primary (within same GPU assumption)
            auto itPrimary = std::find_if(newDisplays.begin(), newDisplays.end(), [](const DisplayInfo& d) { return d.isPrimary; });
            if (itPrimary != newDisplays.end()) {
                newSelectedSerial = itPrimary->serialNumber;
                DebugLog("MonitorDisplayChanges: OS primary changed. Set selection to primary.");
            } else if (selectionStillExists) {
                newSelectedSerial = lastSelectedSerial;
            } else if (!newDisplays.empty()) {
                newSelectedSerial = newDisplays[0].serialNumber;
            }
        } else {
            // Preserve user selection when possible; on unplug, prefer primary else first
            if (selectionStillExists) {
                newSelectedSerial = lastSelectedSerial;
            } else {
                auto itPrimary = std::find_if(newDisplays.begin(), newDisplays.end(), [](const DisplayInfo& d) { return d.isPrimary; });
                if (itPrimary != newDisplays.end()) newSelectedSerial = itPrimary->serialNumber;
                else if (!newDisplays.empty()) newSelectedSerial = newDisplays[0].serialNumber;
            }
        }

        // Now, update everything: shared memory and registry (port order only)
        // Shared Memory
        sharedMemoryHelper.WriteSharedMemory("DISP_INFO_NUM", std::to_string(newDisplays.size()));
        for (size_t i = 0; i < newDisplays.size(); ++i) {
            std::string key = "DISP_INFO_" + std::to_string(i);
            sharedMemoryHelper.WriteSharedMemory(key, newDisplays[i].serialNumber);
        }
        if (!newSelectedSerial.empty()) {
            sharedMemoryHelper.WriteSharedMemory("DISP_INFO", newSelectedSerial);
        }

        // Pulse REBOOT=1 for 1 second on unplug or plug-in, then reset to 0
        if (removalDetected || additionDetected) {
            DebugLog("MonitorDisplayChanges: Plug/unplug detected. Pulsing REBOOT to 1 for 1 second.");
            sharedMemoryHelper.WriteSharedMemory("REBOOT", "1");
            TaskTrayApp* appPtr = this;
            std::thread([appPtr]() {
                Sleep(1000);
                SharedMemoryHelper helper(appPtr);
                helper.WriteSharedMemory("REBOOT", "0");
            }).detach();
        }

        // Registry (SerialNumber1..n in port order)
        RegistryHelper::ClearDISPInfoFromRegistry();
        for (size_t i = 0; i < newDisplays.size(); ++i) {
            RegistryHelper::WriteDISPInfoToRegistryAt(i + 1, newDisplays[i].serialNumber);
        }
        if (!newSelectedSerial.empty()) {
            RegistryHelper::WriteSelectedSerialToRegistry(newSelectedSerial);
        }

        DebugLog("MonitorDisplayChanges: Updated registry and shared memory. New selection: " + newSelectedSerial);

        // Post message to main thread to update UI
        PostMessage(hwnd, WM_USER + 2, 0, 0); // WM_USER + 2 signals a UI refresh
    }
}


void TaskTrayApp::RefreshDisplayList() {
    DebugLog("RefreshDisplayList: Starting display list refresh.");

    // Get selected GPU info from shared memory
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
    if (gpuInfo.empty()) {
        DebugLog("RefreshDisplayList: GPU_INFO is empty. Cannot refresh display list.");
        MessageBox(hwnd, _T("GPU information not found. The application will now exit."), _T("Error"), MB_OK | MB_ICONERROR);
        PostQuitMessage(0);
        return;
    }
    DebugLog("RefreshDisplayList: Read GPU_INFO: " + gpuInfo);

    std::string gpuVendorID, gpuDeviceID;
    size_t delimiter = gpuInfo.find(":");
    if (delimiter != std::string::npos) {
        gpuVendorID = gpuInfo.substr(0, delimiter);
        gpuDeviceID = gpuInfo.substr(delimiter + 1);
    }
    else {
        DebugLog("RefreshDisplayList: Invalid GPU_INFO format.");
        return;
    }

    // Get displays sorted by Windows display number (DISPLAY1, DISPLAY2, ...)
    std::vector<DisplayInfo> newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
    DebugLog("RefreshDisplayList: Found " + std::to_string(newDisplays.size()) + " displays.");

    // Update shared memory with the display list (in Windows display-number order)
    sharedMemoryHelper.WriteSharedMemory("DISP_INFO_NUM", std::to_string(newDisplays.size()));
    for (size_t i = 0; i < newDisplays.size(); ++i) {
        std::string key = "DISP_INFO_" + std::to_string(i);
        sharedMemoryHelper.WriteSharedMemory(key, newDisplays[i].serialNumber);
    }

    // Determine the initial selection.
    // First, get the system-wide primary display.
    std::string systemPrimarySerial = DisplayManager::GetSystemPrimaryDisplaySerial();
    // Cache last known OS primary monitor serial for change detection in the monitor thread
    lastSystemPrimarySerial = systemPrimarySerial;
    std::string primaryDisplaySerial = "";

    // Check if the system primary is in our list of displays for the selected GPU.
    bool primaryFoundInList = false;
    if (!systemPrimarySerial.empty()) {
        for (const auto& display : newDisplays) {
            if (display.serialNumber == systemPrimarySerial) {
                primaryDisplaySerial = systemPrimarySerial;
                primaryFoundInList = true;
                DebugLog("RefreshDisplayList: System primary display is on the selected GPU. Using it for initial selection.");
                break;
            }
        }
    }

    // If the system primary wasn't found in our list (or if there's no system primary),
    // fall back to the first display in the list as per original behavior.
    if (!primaryFoundInList && !newDisplays.empty()) {
        primaryDisplaySerial = newDisplays[0].serialNumber;
        DebugLog("RefreshDisplayList: System primary not on this GPU or not found. Falling back to the first display.");
    }

    // Set initial selection to the primary display
    if (!primaryDisplaySerial.empty()) {
        sharedMemoryHelper.WriteSharedMemory("DISP_INFO", primaryDisplaySerial);
        RegistryHelper::WriteSelectedSerialToRegistry(primaryDisplaySerial);
        DebugLog("RefreshDisplayList: Set initial selected display to primary: " + primaryDisplaySerial);

        // Also update the tooltip
        int selectedIndex = -1;
        for (size_t i = 0; i < newDisplays.size(); ++i) {
            if (newDisplays[i].serialNumber == primaryDisplaySerial) {
                selectedIndex = i + 1;
                break;
            }
        }
        if (selectedIndex != -1) {
            std::wstring initialTooltip = L"Display Manager - Selected: Display " + std::to_wstring(selectedIndex);
            UpdateTrayTooltip(initialTooltip);
        }
    }

    // Update the registry with the display list (Windows display-number order)
    RegistryHelper::ClearDISPInfoFromRegistry();
    for (size_t i = 0; i < newDisplays.size(); ++i) {
        RegistryHelper::WriteDISPInfoToRegistryAt(i + 1, newDisplays[i].serialNumber);
    }

    DebugLog("RefreshDisplayList: Display list refresh complete.");
}

int TaskTrayApp::Run() {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

void TaskTrayApp::UpdateTrayTooltip(const std::wstring& text) {
    DebugLog("UpdateTrayTooltip: Setting tooltip to: " + utf16_to_utf8(text));
    // Copy the new text to the tooltip member of the NOTIFYICONDATA struct.
    // lstrcpyn is a safe way to copy, preventing buffer overflows.
    lstrcpyn(nid.szTip, text.c_str(), _countof(nid.szTip));

    // The uFlags member must specify that the tip is being updated.
    // Also include NIF_INFO to make sure the tooltip is shown as a balloon notification if needed,
    // though here we are just updating the hover tip.
    nid.uFlags = NIF_TIP;

    // Modify the tray icon with the new tooltip.
    if (!Shell_NotifyIcon(NIM_MODIFY, &nid)) {
        DebugLog("UpdateTrayTooltip: Shell_NotifyIcon failed.");
    }
}

void TaskTrayApp::UpdateCaptureModeMenu(HMENU hMenu) {
    while (GetMenuItemCount(hMenu) > 0) {
        RemoveMenu(hMenu, 0, MF_BYPOSITION);
    }

    SharedMemoryHelper sharedMemoryHelper(this);
    std::string captureModeStr = sharedMemoryHelper.ReadSharedMemory("Capture_Mode");
    int captureMode = 1;

    if (!captureModeStr.empty()) {
        try {
            captureMode = std::stoi(captureModeStr);
        }
        catch (...) {
            captureMode = 1;
        }
    }

    UINT normalFlags = MF_STRING;
    UINT gameFlags = MF_STRING;

    if (captureMode == 2) {
        gameFlags |= MF_CHECKED;
    }
    else {
        normalFlags |= MF_CHECKED;
    }

    AppendMenu(hMenu, normalFlags, ID_CAPTURE_MODE_NORMAL, _T("Normal Mode"));
    AppendMenu(hMenu, gameFlags, ID_CAPTURE_MODE_GAME, _T("Game Mode"));
}

