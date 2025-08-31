#include "TaskTrayApp.h"
#include "StringConversion.h"
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
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <dbt.h>


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

    // debuglog.log をバックアップして削除する
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
        std::string backupFileName = timestamp + "_debuglog_tasktray.log.bak";
        std::filesystem::path backupFilePath = exePath / backupFileName;

        // ファイルをリネーム
        std::filesystem::rename(logFilePath, backupFilePath);
    }

    // 3ヶ月以上経過したバックアップファイルを削除する
    auto now = std::chrono::system_clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(exePath)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.size() == 22 && filename.substr(14) == "_debuglog_tasktray.log.bak") {
                std::tm tm = {};
                std::istringstream iss(filename.substr(0, 14));
                iss >> std::get_time(&tm, "%Y%m%d%H%M%S");
                if (!iss.fail()) {
                    auto file_time = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                    auto age = std::chrono::duration_cast<std::chrono::hours>(now - file_time).count();
                    if (age > 24 * 90) { // 3ヶ月以上経過
                        std::filesystem::remove(entry.path());
                    }
                }
            }
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
        DebugLog("Error: hwnd is nullptr.");
        return;
    }

    try {
        POINT pt;
        if (!GetCursorPos(&pt)) {
            DebugLog("Error: Failed to get cursor position.");
            return;
        }
        DebugLog("Cursor position obtained.");

        HMENU hMenu = CreatePopupMenu();
        if (hMenu == NULL) {
            DebugLog("Error: Failed to create popup menu.");
            return;
        }
        DebugLog("Popup menu created.");

        AppendMenu(hMenu, MF_STRING, 1, _T("終了"));
        DebugLog("Exit menu item added.");

        // 「ディスプレイの選択」メニューを追加
        HMENU hSubMenu = CreatePopupMenu();
        if (hSubMenu == NULL) {
            DebugLog("Error: Failed to create submenu.");
            DestroyMenu(hMenu);
            return;
        }
        DebugLog("Submenu created.");

        UpdateDisplayMenu(hSubMenu);
        DebugLog("Display menu updated.");

        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, _T("ディスプレイの選択"));
        DebugLog("Display selection menu added.");

        SetForegroundWindow(hwnd);
        DebugLog("Foreground window set.");

        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DebugLog("Popup menu tracked.");

        DestroyMenu(hMenu);
        DebugLog("Popup menu destroyed.");
    }
    catch (const std::exception& e) {
        DebugLog(std::string("Exception: ") + e.what());
    }
}

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu) {
    DebugLog("UpdateDisplayMenu: Start updating display menu.");

    // 既存のメニュー項目を削除
    while (RemoveMenu(hMenu, 0, MF_BYPOSITION));

    // Get the ordered list from shared memory (with fallback to registry)
    std::vector<std::string> displays = GetDisplayListFromSharedMemory();
    if (displays.empty()) {
        DebugLog("UpdateDisplayMenu: No displays found.");
        AppendMenu(hMenu, MF_STRING | MF_GRAYED, -1, _T("No displays available"));
        return;
    }

    // 現在選択されているディスプレイを取得
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string selectedDisplaySerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    DebugLog("Selected display serial: " + selectedDisplaySerial);

    for (size_t i = 0; i < displays.size(); ++i) {
        UINT flags = MF_STRING;
        if (displays[i] == selectedDisplaySerial) {
            flags |= MF_CHECKED;
        }
        // Using the serial number as the menu item label.
        std::wstring displayName = utf8_to_utf16(displays[i]);
        if (AppendMenu(hMenu, flags, 100 + i, displayName.c_str())) {
            DebugLog("Successfully added menu item: " + displays[i]);
        }
        else {
            DebugLog("Failed to add menu item: " + displays[i]);
        }
    }

    DebugLog("UpdateDisplayMenu: Finished updating display menu.");
}

void TaskTrayApp::SelectDisplay(int displayIndex) {
    std::vector<std::string> displaysList = GetDisplayListFromSharedMemory();
    if (displayIndex < 0 || displayIndex >= displaysList.size()) {
        DebugLog("SelectDisplay: Invalid display index: " + std::to_string(displayIndex));
        return;
    }

    const std::string& selectedSerial = displaysList[displayIndex];

    // 選択されたディスプレイのシリアルナンバーを共有メモリに保存
    // This key is used by UpdateDisplayMenu to set the checkmark.
    SharedMemoryHelper sharedMemoryHelper(this);
    if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO", selectedSerial)) {
        DebugLog("SelectDisplay: Failed to write display serial number to shared memory.");
        return;
    }
    DebugLog("SelectDisplay: Display serial number written to shared memory: " + selectedSerial);

    // Also persist this choice to the registry
    if (!RegistryHelper::WriteSelectedDisplaySerial(selectedSerial)) {
        DebugLog("SelectDisplay: Failed to write selected serial to registry.");
    } else {
        DebugLog("SelectDisplay: Successfully wrote selected serial to registry.");
    }

    // The menu will be correctly updated the next time it is opened.
}

LRESULT CALLBACK TaskTrayApp::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    TaskTrayApp* app = reinterpret_cast<TaskTrayApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    if (app == nullptr && uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        app = reinterpret_cast<TaskTrayApp*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        DebugLog("WindowProc: WM_CREATE - TaskTrayApp instance set.");
    }

    if (app) {
        bool cleanupResult = false; // 初期化
        HMENU hMenu = NULL; // ここで hMenu を宣言
        switch (uMsg) {
        case WM_USER + 1://タスクトレイアイコンを右クリックしたとき
            if (lParam == WM_RBUTTONUP) {
                DebugLog("WindowProc: WM_USER + 1 - Right button up.");
                app->ShowContextMenu(); // グローバル変数 displays を引数として渡す
            }
            break;
        case WM_USER + 2://ディスプレイの接続状況に変化があったとき
            DebugLog("WindowProc: WM_USER + 2 - Display change notification received. Menu will be updated on next right-click.");
            // The menu is built on-demand by ShowContextMenu. We just need to receive the signal.
            // The old code here was building and destroying a menu for no reason.
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == 1) {//終了ボタンをクリックしたとき
                DebugLog("WindowProc: WM_COMMAND - Exit command received.");
                cleanupResult = app->Cleanup();
                if (cleanupResult) {
                    DebugLog("WindowProc: Cleanup succeeded.");
                    PostQuitMessage(0);
                }
                else {
                    DebugLog("WindowProc: Cleanup failed.");
                }
                
            }
            else if (LOWORD(wParam) >= 100) {//ディスプレイを選択したとき
                DebugLog("WindowProc: WM_COMMAND - Display selection command received.");
                app->SelectDisplay(LOWORD(wParam) - 100);
            }
            break;
        default:
            DebugLog("WindowProc: Default case - Message received: " + std::to_string(uMsg));
            break;
        }
    }
    else {
        DebugLog("WindowProc: TaskTrayApp instance is null.");
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}


// Helper function to get the ordered display list, preferring shared memory
std::vector<std::string> TaskTrayApp::GetDisplayListFromSharedMemory() {
    std::vector<std::string> displays;
    SharedMemoryHelper sharedMemoryHelper(this);
    for (int i = 0; ; ++i) {
        std::string key = "DISP_INFO_" + std::to_string(i);
        std::string serial = sharedMemoryHelper.ReadSharedMemory(key);
        if (serial.empty() || serial.length() > 250) { // Basic sanity check
            break; // No more displays or invalid data
        }
        displays.push_back(serial);
    }

    // Fallback if shared memory is empty but shouldn't be
    if (displays.empty()) {
        DebugLog("GetDisplayListFromSharedMemory: Shared memory was empty, falling back to registry.");
        displays = RegistryHelper::ReadDISPInfoFromRegistry();
        // Here we lose the guaranteed order, but it's a fallback.
    }
    return displays;
}

void TaskTrayApp::MonitorDisplayChanges() {
    // The cache is initialized by RefreshDisplayList on startup.
    while (running) {
        Sleep(2000); // 2秒ごとに監視

        SharedMemoryHelper sharedMemoryHelper(this);
        std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
        if (gpuInfo.empty()) {
            DebugLog("MonitorDisplayChanges: GPU_INFO is empty, skipping check.");
            continue;
        }

        std::string gpuVendorID, gpuDeviceID;
        size_t delimiter = gpuInfo.find(":");
        if (delimiter != std::string::npos) {
            gpuVendorID = gpuInfo.substr(0, delimiter);
            gpuDeviceID = gpuInfo.substr(delimiter + 1);
        } else {
            continue;
        }

        auto newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);

        std::vector<std::string> currentSerials;
        for (const auto& display : newDisplays) {
            if (!display.serialNumber.empty()) {
                currentSerials.push_back(display.serialNumber);
            }
        }

        if (cachedDisplaySerials != currentSerials) {
            DebugLog("MonitorDisplayChanges: Change detected in display list.");
            cachedDisplaySerials = currentSerials;

            std::string primarySerial = "";
            std::string primaryName = "N/A";
            if (!newDisplays.empty()) {
                primarySerial = newDisplays[0].serialNumber; // First is always primary after sort
                primaryName = newDisplays[0].name;
            }

            // Write legacy DISP_INFO
            sharedMemoryHelper.WriteSharedMemory("DISP_INFO", primarySerial);

            // Write new ordered DISP_INFO_0..N
            size_t i = 0;
            for (i = 0; i < newDisplays.size(); ++i) {
                std::string key = "DISP_INFO_" + std::to_string(i);
                sharedMemoryHelper.WriteSharedMemory(key, newDisplays[i].serialNumber);
            }
            // Write a terminator entry to mark the end of the list
            sharedMemoryHelper.WriteSharedMemory("DISP_INFO_" + std::to_string(i), "");

            // Clear and rewrite registry
            HKEY hKey;
            if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL) == ERROR_SUCCESS) {
                char valueName[256];
                DWORD valueNameSize = sizeof(valueName);
                while (RegEnumValueA(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                    RegDeleteValueA(hKey, valueName);
                    valueNameSize = sizeof(valueName);
                }
                RegCloseKey(hKey);
            }

            ::serialNumberIndex = 0; // Reset global index
            for (const auto& serial : currentSerials) {
                RegistryHelper::WriteDISPInfoToRegistry(serial);
            }

            // Update tooltip
            std::wstring tipString = L"GPU & Display Manager - Primary: " + utf8_to_utf16(primaryName);
            lstrcpyn(nid.szTip, tipString.c_str(), sizeof(nid.szTip) / sizeof(TCHAR));
            Shell_NotifyIcon(NIM_MODIFY, &nid);
            DebugLog("MonitorDisplayChanges: Updated tooltip.");

            // Notify main thread to update menu
            PostMessage(hwnd, WM_USER + 2, 0, 0);
            DebugLog("MonitorDisplayChanges: Posted message to update menu.");
        }
    }
}


void TaskTrayApp::RefreshDisplayList() {
    // GPUの決定
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
    if (gpuInfo.empty()) {
        DebugLog("RefreshDisplayList: Failed to read GPU_INFO or GPU_INFO is empty.");
        int result = MessageBox(hwnd, _T("メモリーから情報を読み取れません。製造元に問い合わせてください。アプリケーションを終了します。"), _T("エラー"), MB_OK | MB_ICONERROR);
        if (result == IDOK) {
            PostQuitMessage(0);
        }
        return;
    }
    DebugLog("RefreshDisplayList: Read GPU_INFO: " + gpuInfo);

    std::string gpuVendorID, gpuDeviceID;
    size_t delimiter = gpuInfo.find(":");
    if (delimiter != std::string::npos) {
        gpuVendorID = gpuInfo.substr(0, delimiter);
        gpuDeviceID = gpuInfo.substr(delimiter + 1);
    } else {
        DebugLog("RefreshDisplayList: Invalid GPU_INFO format.");
        return;
    }

    // ディスプレイ情報を取得する
    DebugLog("RefreshDisplayList: Calling GetDisplaysForGPU with VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    std::vector<DisplayInfo> newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
    if (newDisplays.empty()) {
        DebugLog("RefreshDisplayList: Failed to retrieve display list or no displays found.");
        return;
    }

    std::string primarySerial = "";
    for (const auto& display : newDisplays) {
        if (display.isPrimary) {
            primarySerial = display.serialNumber;
            break;
        }
    }
    if (primarySerial.empty() && !newDisplays.empty()) {
        primarySerial = newDisplays[0].serialNumber; // Fallback to first in list
        DebugLog("RefreshDisplayList: No primary display found, falling back to first display in sorted list.");
    }

    // Write legacy DISP_INFO for backward compatibility
    sharedMemoryHelper.WriteSharedMemory("DISP_INFO", primarySerial);
    DebugLog("RefreshDisplayList: Wrote legacy DISP_INFO with serial: " + primarySerial);

    // Write new ordered DISP_INFO_0..N to shared memory
    for (size_t i = 0; i < newDisplays.size(); ++i) {
        std::string key = "DISP_INFO_" + std::to_string(i);
        sharedMemoryHelper.WriteSharedMemory(key, newDisplays[i].serialNumber);
    }

    // Clear existing registry values under the DISP path
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        // This is a more robust way to delete all values
        char valueName[256];
        DWORD valueNameSize = sizeof(valueName);
        while (RegEnumValueA(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            RegDeleteValueA(hKey, valueName);
            valueNameSize = sizeof(valueName); // Reset for next iteration
        }
        RegCloseKey(hKey);
    } else {
        DebugLog("RefreshDisplayList: Failed to open registry key to clear old values.");
    }

    // Write new ordered SerialNumber0..N to registry
    ::serialNumberIndex = 0; // Reset global index
    cachedDisplaySerials.clear();
    for (const auto& display : newDisplays) {
        if (!RegistryHelper::WriteDISPInfoToRegistry(display.serialNumber)) {
            DebugLog("RefreshDisplayList: Failed to write display serial to registry: " + display.serialNumber);
        }
        cachedDisplaySerials.push_back(display.serialNumber);
    }
    DebugLog("RefreshDisplayList: Wrote new ordered display list to shared memory and registry.");
}

int TaskTrayApp::Run() {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}


