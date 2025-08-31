#include "TaskTrayApp.h"

// Use an app-range message for tray callbacks (safer than WM_USER+n)
#ifndef WMAPP_TRAYICON
#define WMAPP_TRAYICON (WM_APP + 1)
#endif

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
    UpdateWindow(hwnd);

    uTaskbarCreatedMsg = RegisterWindowMessage(L"TaskbarCreated");

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
    nid.uCallbackMessage = WMAPP_TRAYICON;  // was WM_USER + 1
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpy(nid.szTip, _T("GPU & Display Manager"));
    if (!Shell_NotifyIcon(NIM_ADD, &nid)) {
        DebugLog("CreateTrayIcon: Shell_NotifyIcon(NIM_ADD) failed. GetLastError=" + std::to_string(GetLastError()));
    } else {
        nid.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIcon(NIM_SETVERSION, &nid)) {
            DebugLog("CreateTrayIcon: Shell_NotifyIcon(NIM_SETVERSION) failed. GetLastError=" + std::to_string(GetLastError()));
        }
    }
}

void TaskTrayApp::RecreateTrayIcon() {
    Shell_NotifyIcon(NIM_DELETE, &nid);
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WMAPP_TRAYICON;  // was WM_USER + 1
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpy(nid.szTip, _T("GPU & Display Manager"));
    if (!Shell_NotifyIcon(NIM_ADD, &nid)) {
        DebugLog("RecreateTrayIcon: Shell_NotifyIcon(NIM_ADD) failed. GetLastError=" + std::to_string(GetLastError()));
        return;
    }

    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIcon(NIM_SETVERSION, &nid)) {
        DebugLog("RecreateTrayIcon: Shell_NotifyIcon(NIM_SETVERSION) failed. GetLastError=" + std::to_string(GetLastError()));
    }
    DebugLog("RecreateTrayIcon: Tray icon recreated and version set.");
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
    std::vector<std::string> DisplaysList = RegistryHelper::ReadDISPInfoFromRegistry();
    if (hwnd == nullptr) {
        DebugLog("Error: hwnd is nullptr.");
        return;
    }

    try {
        if (DisplaysList.empty()) {
            DebugLog("Error: Display information is not available.");
            MessageBox(hwnd, _T("ディスプレイ情報が取得されていません。"), _T("エラー"), MB_OK | MB_ICONERROR);
            return;
        }

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
        if (!DisplaysList.empty()) {
            HMENU hSubMenu = CreatePopupMenu();
            if (hSubMenu == NULL) {
                DebugLog("Error: Failed to create submenu.");
                DestroyMenu(hMenu);
                return;
            }
            DebugLog("Submenu created.");

            UpdateDisplayMenu(hSubMenu, DisplaysList);
            DebugLog("Display menu updated.");

            AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, _T("ディスプレイの選択"));
            DebugLog("Display selection menu added.");
        }

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

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu, const std::vector<std::string>& /*unused*/) {
    DebugLog("UpdateDisplayMenu: Rebuilding from shared memory order.");
    while (RemoveMenu(hMenu, 0, MF_BYPOSITION));

    SharedMemoryHelper shm(this);
    // Authoritative order for display items
    std::vector<std::string> ordered = shm.ReadDisplayList();

    // Currently selected display (keep your existing selection behavior)
    std::string selected = shm.ReadSharedMemory("DISP_INFO");
    DebugLog("UpdateDisplayMenu: Selected display serial: " + selected);

    for (size_t i = 0; i < ordered.size(); ++i) {
        UINT flags = MF_STRING;
        if (ordered[i] == selected) flags |= MF_CHECKED;

        std::wstring wlabel(ordered[i].begin(), ordered[i].end());
        if (AppendMenu(hMenu, flags, 100 + (UINT)i, wlabel.c_str())) {
            DebugLog("UpdateDisplayMenu: Added: " + ordered[i]);
        } else {
            DebugLog("UpdateDisplayMenu: Failed to add: " + ordered[i]);
        }
    }
    DebugLog("UpdateDisplayMenu: Done.");
}

void TaskTrayApp::SelectDisplay(int displayIndex) {
    std::vector<std::string> DisplaysList = RegistryHelper::ReadDISPInfoFromRegistry();
    if (displayIndex < 0 || displayIndex >= DisplaysList.size()) {
        DebugLog("SelectDisplay: Invalid display index: " + std::to_string(displayIndex));
        return;
    }

    // 選択されたディスプレイのシリアルナンバーを共有メモリに保存
    SharedMemoryHelper sharedMemoryHelper(this);
    if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO", DisplaysList[displayIndex])) {
        DebugLog("SelectDisplay: Failed to write display serial number to shared memory.");
        return;
    }
    DebugLog("SelectDisplay: Display serial number written to shared memory: " + DisplaysList[displayIndex]);

    // メニューを更新し、新しく選択したディスプレイにチェックマークをつける
    HMENU hMenu = CreatePopupMenu();
    if (hMenu == NULL) {
        DebugLog("SelectDisplay: Failed to create popup menu.");
        return;
    }
    UpdateDisplayMenu(hMenu, DisplaysList);
    DestroyMenu(hMenu);
    DebugLog("SelectDisplay: Display menu updated.");
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
        HMENU hMenu = NULL; // ここで hMenu を宣言
        switch (uMsg) {
        case WMAPP_TRAYICON: // tray callback message
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                DebugLog("WindowProc: Tray callback - context menu requested.");
                app->ShowContextMenu();
            }
            break;
        case WM_USER + 2://ディスプレイの接続状況に変化があったとき
            DebugLog("WindowProc: WM_USER + 2 - Update display menu.");
            hMenu = CreatePopupMenu(); // ここで hMenu を初期化
            if (hMenu != NULL) {
                std::vector<std::string> DisplaysList = RegistryHelper::ReadDISPInfoFromRegistry();
                app->UpdateDisplayMenu(hMenu, DisplaysList);
                DestroyMenu(hMenu);
                DebugLog("WindowProc: Display menu updated.");
            }
            else {
                DebugLog("WindowProc: Failed to create task tray menu.");
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == 1) {//終了ボタンをクリックしたとき
                DebugLog("WindowProc: WM_COMMAND - Exit command received.");
                app->Cleanup();
                DebugLog("WindowProc: Cleanup succeeded.");
                PostQuitMessage(0);
            }
            else if (LOWORD(wParam) >= 100) {//ディスプレイを選択したとき
                DebugLog("WindowProc: WM_COMMAND - Display selection command received.");
                app->SelectDisplay(LOWORD(wParam) - 100);
            }
            break;
        // Re-add after display topology/primary change
        case WM_DISPLAYCHANGE:
            DebugLog("WindowProc: WM_DISPLAYCHANGE - Re-enumerate displays and recreate tray icon.");
            app->RefreshDisplayList();
            app->RecreateTrayIcon();
            break;

        // Re-add after explorer/taskbar recreation
        default: {
            if (uMsg == app->uTaskbarCreatedMsg) {
                DebugLog("WindowProc: TaskbarCreated - Recreate tray icon, refresh menu.");
                app->RecreateTrayIcon();
                app->RefreshDisplayList();
                break;
            }
            DebugLog("WindowProc: Default case - Message received: " + std::to_string(uMsg));
            break;
        }
        }
    }
    else {
        DebugLog("WindowProc: TaskTrayApp instance is null.");
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}


void TaskTrayApp::MonitorDisplayChanges() {
    while (running) {
        Sleep(2000); // 2秒ごとに監視

        // ディスプレイ情報を取得
        SharedMemoryHelper sharedMemoryHelper(this);
        std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
        DebugLog("MonitorDisplayChanges: Read GPU_INFO: " + gpuInfo);

        std::string gpuVendorID, gpuDeviceID;
        size_t delimiter = gpuInfo.find(":");
        if (delimiter != std::string::npos) {
            gpuVendorID = gpuInfo.substr(0, delimiter);
            gpuDeviceID = gpuInfo.substr(delimiter + 1);
        }

        if (gpuVendorID.empty() || gpuDeviceID.empty()) {
            DebugLog("MonitorDisplayChanges: GPU_INFO is empty!");
            continue;  // ディスプレイリストの更新をスキップs
        }

        auto NewDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
        DebugLog("MonitorDisplayChanges: Retrieved new display list.");

        std::vector<std::string> Displays_Before = RegistryHelper::ReadDISPInfoFromRegistry();

		std::vector<std::string> Displays_After;
        for (const auto& display : NewDisplays) {
            if (!display.serialNumber.empty()) {
				Displays_After.push_back(display.serialNumber);
            }
        }


        if (Displays_Before != Displays_After) {
            DebugLog("MonitorDisplayChanges: Display configuration changed.");

            // Displays_After is the new, ordered list of serials.
            // 1) Write list to shared memory
            if (!sharedMemoryHelper.WriteDisplayList(Displays_After)) {
                DebugLog("MonitorDisplayChanges: WriteDisplayList failed.");
            }

            // 2) Maintain DISP_INFO for compat
            if (!Displays_After.empty()) {
                if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO", Displays_After[0])) { // primary is index 0
                    DebugLog("MonitorDisplayChanges: Failed to write DISP_INFO (primary).");
                }
            }

            // 3) Rewrite registry
            HKEY hKey;
            if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
                // Delete existing values
                DWORD valueCount = 0;
                RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &valueCount, NULL, NULL, NULL, NULL);
                for (DWORD i = 0; i < valueCount; ++i) {
                    wchar_t valueName[256];
                    DWORD valueNameSize = sizeof(valueName) / sizeof(valueName[0]);
                    if (RegEnumValue(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        RegDeleteValue(hKey, valueName);
                    }
                }
                RegCloseKey(hKey); // Close key after use
            }
            else {
                DebugLog("MonitorDisplayChanges: Failed to create or open registry key for clearing.");
            }

            ::serialNumberIndex = 0;
            for (const auto& serial : Displays_After) {
                if (!RegistryHelper::WriteDISPInfoToRegistry(serial)) {
                    DebugLog("MonitorDisplayChanges: Failed to write serial to registry: " + serial);
                }
            }
            DebugLog("MonitorDisplayChanges: Registry updated.");

            // Notify UI thread to rebuild menu
            PostMessage(hwnd, WM_USER + 2, 0, 0);
        }
    }
}


void TaskTrayApp::RefreshDisplayList() {
    SharedMemoryHelper shm(this);
    std::string gpuInfo = shm.ReadSharedMemory("GPU_INFO");
    if (gpuInfo.empty()) {
        DebugLog("RefreshDisplayList: Failed to read GPU_INFO or GPU_INFO is empty.");
        int result = MessageBox(hwnd, _T("メモリーから情報を読み取れません。製造元に問い合わせてください。アプリケーションを終了します。"), _T("エラー"), MB_OK | MB_ICONERROR);
        if (result == IDOK) PostQuitMessage(0);
        return;
    }
    DebugLog("RefreshDisplayList: Read GPU_INFO: " + gpuInfo);

    std::string gpuVendorID, gpuDeviceID;
    size_t pos = gpuInfo.find(':');
    if (pos == std::string::npos) {
        DebugLog("RefreshDisplayList: Invalid GPU_INFO format.");
        return;
    }
    gpuVendorID = gpuInfo.substr(0, pos);
    gpuDeviceID = gpuInfo.substr(pos + 1);

    // Enumerate displays primary-first then port order
    std::vector<DisplayInfo> newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
    if (newDisplays.empty()) {
        DebugLog("RefreshDisplayList: No displays found.");
        return;
    }

    // Build serial list in required order
    std::vector<std::string> serials;
    serials.reserve(newDisplays.size());
    for (const auto& d : newDisplays) serials.push_back(d.serialNumber);

    // 1) Write list to shared memory: DISP_INFO_0..N
    if (!shm.WriteDisplayList(serials)) {
        DebugLog("RefreshDisplayList: WriteDisplayList failed.");
    }

    // 2) Maintain existing selected-display key for backward compat
    if (!shm.WriteSharedMemory("DISP_INFO", serials[0])) { // primary is index 0
        DebugLog("RefreshDisplayList: Failed to write DISP_INFO (primary).");
    }

    // 3) Rewrite registry SerialNumber0.. in the same order
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        // Delete existing values
        DWORD valueCount = 0;
        RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &valueCount, NULL, NULL, NULL, NULL);
        for (DWORD i = 0; i < valueCount; ++i) {
            wchar_t valueName[256]; DWORD valueNameSize = _countof(valueName);
            if (RegEnumValue(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                RegDeleteValue(hKey, valueName);
            }
        }
        RegCloseKey(hKey);
    } else {
        DebugLog("RefreshDisplayList: Failed to open registry key for clearing.");
    }

    ::serialNumberIndex = 0;
    for (const auto& s : serials) {
        if (!RegistryHelper::WriteDISPInfoToRegistry(s)) {
            DebugLog("RefreshDisplayList: Failed to write SerialNumber to registry.");
        }
    }
    DebugLog("RefreshDisplayList: Display list written (shared memory + registry).");

    // Notify UI thread to rebuild menu
    PostMessage(hwnd, WM_USER + 2, 0, 0);
}

int TaskTrayApp::Run() {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}


