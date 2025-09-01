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

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu, const std::vector<std::string>& displays) {
    DebugLog("UpdateDisplayMenu: Start.");
    while (RemoveMenu(hMenu, 0, MF_BYPOSITION));

    SharedMemoryHelper shm(this);
    std::string selectedSerial = shm.ReadSharedMemory("DISP_INFO");
    if (selectedSerial.empty()) {
        selectedSerial = RegistryHelper::ReadSelectedDisplayFromRegistry();
        DebugLog("UpdateDisplayMenu: SHM returned empty; using registry SelectedSerial: " + selectedSerial);
    }
    DebugLog("UpdateDisplayMenu: Selected=" + selectedSerial);

    for (size_t i = 0; i < displays.size(); ++i) {
        UINT flags = MF_STRING;
        if (displays[i] == selectedSerial) flags |= MF_CHECKED;

        std::wstring caption = L"Display" + std::to_wstring(i + 1); // 1-based
        if (AppendMenu(hMenu, flags, 100 + (UINT)i, caption.c_str())) {
            DebugLog("UpdateDisplayMenu: Added " + std::string("Display") + std::to_string(i + 1));
        } else {
            DebugLog("UpdateDisplayMenu: Failed to add item " + std::to_string(i + 1));
        }
    }
    DebugLog("UpdateDisplayMenu: Done.");
}

void TaskTrayApp::SelectDisplay(int displayIndex) {
    std::vector<std::string> list = RegistryHelper::ReadDISPInfoFromRegistry();
    if (displayIndex < 0 || displayIndex >= (int)list.size()) {
        DebugLog("SelectDisplay: Invalid index.");
        return;
    }
    const std::string& serial = list[displayIndex];

    SharedMemoryHelper shm(this);
    if (!shm.WriteSharedMemory("DISP_INFO", serial)) {
        DebugLog("SelectDisplay: Failed to write DISP_INFO.");
        return;
    }
    DebugLog("SelectDisplay: Wrote DISP_INFO=" + serial);

    (void)RegistryHelper::WriteSelectedDisplayToRegistry(serial);

    // Update tooltip to reflect selection (optional)
    std::wstring tip = L"GPU & Display Manager - Display" + std::to_wstring(displayIndex + 1);
    wcsncpy_s(nid.szTip, _countof(nid.szTip), tip.c_str(), _TRUNCATE);
    nid.uFlags = NIF_TIP;
    Shell_NotifyIcon(NIM_MODIFY, &nid);
    DebugLog("SelectDisplay: Tooltip updated.");

    HMENU hMenu = CreatePopupMenu();
    if (hMenu) {
        UpdateDisplayMenu(hMenu, list);
        DestroyMenu(hMenu);
    }
    DebugLog("SelectDisplay: Menu updated.");
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

        auto NewDisplays = DisplayManager::GetDisplaysForGPUByPortOrder(gpuVendorID, gpuDeviceID);
        DebugLog("MonitorDisplayChanges: Retrieved new display list (port-ordered).");

        // Track primary changes even if list is the same
        static std::string lastPrimarySerial;
        std::string currentPrimarySerial;
        for (const auto& d : NewDisplays) {
            if (d.isPrimary) { currentPrimarySerial = d.serialNumber; break; }
        }

        std::vector<std::string> Displays_Before = RegistryHelper::ReadDISPInfoFromRegistry();

		std::vector<std::string> Displays_After;
        for (const auto& display : NewDisplays) {
            if (!display.serialNumber.empty()) {
				Displays_After.push_back(display.serialNumber);
            }
        }


        if (Displays_Before != Displays_After) {
            DebugLog("MonitorDisplayChanges: Change detected, updating display list.");

            // Rewrite registry values
            HKEY hKey;
            if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
                // 既存の値を削除
                DWORD valueCount = 0;
                RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &valueCount, NULL, NULL, NULL, NULL);
                for (DWORD i = 0; i < valueCount; ++i) {
                    wchar_t valueName[256];
                    DWORD valueNameSize = sizeof(valueName) / sizeof(valueName[0]);
                    if (RegEnumValue(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        RegDeleteValue(hKey, valueName);
                    }
                }
                RegCloseKey(hKey);
            } else {
                DebugLog("MonitorDisplayChanges: Failed to create or open registry key for deletion.");
            }

            ::serialNumberIndex = 0;
            for (const auto& display : NewDisplays) {
                if (!RegistryHelper::WriteDISPInfoToRegistry(display.serialNumber)) {
                    DebugLog("MonitorDisplayChanges: Failed to write display serial number to registry.");
                }
            }

            // Write shared memory keys
            SharedMemoryHelper shm(this);
            shm.WriteSharedMemory("DISP_INFO_NUM", std::to_string((int)NewDisplays.size()));
            for (size_t i = 0; i < NewDisplays.size(); ++i) {
                shm.WriteSharedMemory("DISP_INFO_" + std::to_string(i + 1), NewDisplays[i].serialNumber);
            }

            // Keep selection if still present, else use primary
            std::string prevSel = shm.ReadSharedMemory("DISP_INFO");
            std::string newSel = "";
            bool prevStillExists = std::any_of(NewDisplays.begin(), NewDisplays.end(),
                [&](const DisplayInfo& d){ return d.serialNumber == prevSel; });

            if (prevStillExists) {
                newSel = prevSel;
                DebugLog("MonitorDisplayChanges: Preserving existing selection: " + newSel);
            } else {
                for (const auto& d : NewDisplays) { if (d.isPrimary) { newSel = d.serialNumber; break; } }
                DebugLog("MonitorDisplayChanges: Falling back to primary display: " + newSel);
            }
            if (!newSel.empty()) {
                shm.WriteSharedMemory("DISP_INFO", newSel);
                (void)RegistryHelper::WriteSelectedDisplayToRegistry(newSel);
                DebugLog("MonitorDisplayChanges: Wrote new selection to SHM and Registry: " + newSel);
            }

            // メインスレッドにメニューの更新を指示
            PostMessage(hwnd, WM_USER + 2, 0, 0);
        }
        // If the list is identical but OS primary changed, do not overwrite selection.
        // Just refresh menu/tooltip so UI remains consistent.
        if (Displays_Before == Displays_After) {
            if (lastPrimarySerial != currentPrimarySerial) {
                DebugLog("MonitorDisplayChanges: Primary changed (list unchanged). Preserving selection.");
                // Optional: update tooltip to reflect current selected display
                // (Selection remains in SHM 'DISP_INFO'; do not modify it here.)
                PostMessage(hwnd, WM_USER + 2, 0, 0);
            }
        }
        lastPrimarySerial = currentPrimarySerial;
    }
}


void TaskTrayApp::RefreshDisplayList() {
    // GPUの決定
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
    if (gpuInfo.empty()) {
        DebugLog("RefreshDisplayList: Failed to read GPU_INFO or GPU_INFO is empty.");
        // メッセージボックスを表示
        int result = MessageBox(hwnd, _T("メモリーから情報を読み取れません。製造元に問い合わせてください。アプリケーションを終了します。"), _T("エラー"), MB_OK | MB_ICONERROR);
        if (result == IDOK) {
            // アプリケーションを終了
            PostQuitMessage(0);
        }
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
    }

    // ディスプレイ情報を取得する
    DebugLog("RefreshDisplayList: Calling GetDisplaysForGPUByPortOrder with VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    std::vector<DisplayInfo> newDisplays = DisplayManager::GetDisplaysForGPUByPortOrder(gpuVendorID, gpuDeviceID);
    if (newDisplays.empty()) {
        DebugLog("RefreshDisplayList: Failed to retrieve display list or no displays found.");
    }
    else {
        // After newDisplays (port-ordered) is ready:
        SharedMemoryHelper shm(this);
        const int count = (int)newDisplays.size();
        shm.WriteSharedMemory("DISP_INFO_NUM", std::to_string(count));
        DebugLog("RefreshDisplayList: Wrote DISP_INFO_NUM=" + std::to_string(count));
        for (int i = 0; i < count; ++i) {
            std::string key = "DISP_INFO_" + std::to_string(i + 1); // 1-based
            if (!newDisplays[i].serialNumber.empty()) {
                shm.WriteSharedMemory(key, newDisplays[i].serialNumber);
                DebugLog("RefreshDisplayList: Wrote " + key + "=" + newDisplays[i].serialNumber);
            }
        }

        // Prefer persisted selection if available; else fall back to OS primary
        std::vector<std::string> orderedSerials;
        orderedSerials.reserve(count);
        for (int i = 0; i < count; ++i) {
            orderedSerials.push_back(newDisplays[i].serialNumber);
        }

        std::string persistedSel = RegistryHelper::ReadSelectedDisplayFromRegistry();
        bool persistedSelOk = false;
        if (!persistedSel.empty()) {
            for (const auto& s : orderedSerials) {
                if (s == persistedSel) { persistedSelOk = true; break; }
            }
        }

        if (persistedSelOk) {
            shm.WriteSharedMemory("DISP_INFO", persistedSel);
            DebugLog("RefreshDisplayList: Restored selection from registry: " + persistedSel);
        } else {
            bool primaryDisplayFound = false;
            for (const auto& d : newDisplays) {
                if (d.isPrimary) {
                    shm.WriteSharedMemory("DISP_INFO", d.serialNumber);
                    (void)RegistryHelper::WriteSelectedDisplayToRegistry(d.serialNumber);
                    DebugLog("RefreshDisplayList: No persisted selection. Using OS primary: " + d.serialNumber);
                    primaryDisplayFound = true;
                    break;
                }
            }
            if (!primaryDisplayFound) {
                DebugLog("RefreshDisplayList: No primary display found at startup.");
            }
        }

        HKEY hKey;
        if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            // 既存の値を削除
            DWORD valueCount = 0;
            RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &valueCount, NULL, NULL, NULL, NULL);
            for (DWORD i = 0; i < valueCount; ++i) {
                wchar_t valueName[256];
                DWORD valueNameSize = sizeof(valueName) / sizeof(valueName[0]);
                if (RegEnumValue(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                    RegDeleteValue(hKey, valueName);
                }
            }
            RegCloseKey(hKey);
        }
        else {
            DebugLog("WriteDISPInfoToRegistry: Failed to create or open registry key.");
        }

        char buffer[10]; // 変換後の文字列を格納するバッファ
        ::serialNumberIndex = 0;
        for (const auto& display : newDisplays) {
            _itoa_s(::serialNumberIndex, buffer, sizeof(buffer), 10); // 変換を行う
            std::string displayInfo = std::string(buffer);
            if (!RegistryHelper::WriteDISPInfoToRegistry(display.serialNumber)) {
                DebugLog("RefreshDisplayList: Failed to write primary display serial number to registry." + displayInfo);
                continue;
            }

            DebugLog("RefreshDisplayList: Primary display serial number saved: " + displayInfo);
            
        }
        DebugLog("RefreshDisplayList: Retrieved display list.");
    }
}

int TaskTrayApp::Run() {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}


