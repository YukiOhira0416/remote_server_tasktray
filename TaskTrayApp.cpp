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

    taskbarCreatedMsg = RegisterWindowMessage(L"TaskbarCreated");

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
        DebugLog("ShowContextMenu: hwnd is nullptr.");
        return;
    }

    SharedMemoryHelper smh(this);
    std::string numDisplaysStr = smh.ReadSharedMemory("DISP_INFO_NUM");
    int numDisplays = 0;
    try {
        if (!numDisplaysStr.empty()) numDisplays = std::stoi(numDisplaysStr);
    }
    catch (...) {}

    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, 1, _T("終了"));

    if (numDisplays > 0) {
        HMENU hSubMenu = CreatePopupMenu();
        UpdateDisplayMenu(hSubMenu);
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, _T("ディスプレイの選択"));
    }
    else {
        AppendMenu(hMenu, MF_STRING | MF_GRAYED, -1, _T("ディスプレイの選択"));
    }

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu) {
    DebugLog("UpdateDisplayMenu: Start updating display menu.");
    while (RemoveMenu(hMenu, 0, MF_BYPOSITION));

    SharedMemoryHelper sharedMemoryHelper(this);
    std::string numDisplaysStr = sharedMemoryHelper.ReadSharedMemory("DISP_INFO_NUM");
    int numDisplays = 0;
    try { if (!numDisplaysStr.empty()) numDisplays = std::stoi(numDisplaysStr); }
    catch (...) {}

    if (numDisplays == 0) {
        AppendMenu(hMenu, MF_STRING | MF_GRAYED, -1, _T("No displays connected"));
        return;
    }

    std::string selectedDisplaySerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    DebugLog("UpdateDisplayMenu: Selected display is " + selectedDisplaySerial);

    for (int i = 0; i < numDisplays; ++i) {
        std::string key = "DISP_INFO_" + std::to_string(i + 1);
        std::string currentSerial = sharedMemoryHelper.ReadSharedMemory(key);

        UINT flags = MF_STRING;
        if (!currentSerial.empty() && currentSerial == selectedDisplaySerial) {
            flags |= MF_CHECKED;
        }

        std::wstring displayName = L"Display " + std::to_wstring(i + 1);
        AppendMenuW(hMenu, flags, 100 + i, displayName.c_str());
    }
    DebugLog("UpdateDisplayMenu: Finished updating display menu.");
}

void TaskTrayApp::SelectDisplay(int displayIndex) {
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string key = "DISP_INFO_" + std::to_string(displayIndex + 1);
    std::string selectedSerial = sharedMemoryHelper.ReadSharedMemory(key);

    if (selectedSerial.empty()) {
        DebugLog("SelectDisplay: Could not find serial for display index: " + std::to_string(displayIndex));
        return;
    }

    sharedMemoryHelper.WriteSharedMemory("DISP_INFO", selectedSerial);
    RegistryHelper::WriteSelectedDisplaySerial(selectedSerial);
    DebugLog("SelectDisplay: New selection '" + selectedSerial + "' saved.");

    UpdateTooltip();
}

LRESULT CALLBACK TaskTrayApp::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    TaskTrayApp* app = reinterpret_cast<TaskTrayApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        app = reinterpret_cast<TaskTrayApp*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        DebugLog("WindowProc: WM_CREATE - TaskTrayApp instance set.");
        return 0;
    }

    if (app) {
        if (uMsg == app->taskbarCreatedMsg) {
            DebugLog("WindowProc: TaskbarCreated message received. Re-creating icon.");
            app->CreateTrayIcon();
            app->UpdateTooltip();
            return 0;
        }

        switch (uMsg) {
        case WM_USER + 1://タスクトレイアイコンを右クリックしたとき
            if (lParam == WM_RBUTTONUP) {
                DebugLog("WindowProc: WM_USER + 1 - Right button up.");
                app->ShowContextMenu();
            }
            break;
        case WM_USER + 2://ディスプレイの接続状況に変化があったとき
            DebugLog("WindowProc: WM_USER + 2 - Display change detected, refreshing tooltip.");
            app->UpdateTooltip();
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == 1) {//終了ボタンをクリックしたとき
                DebugLog("WindowProc: WM_COMMAND - Exit command received.");
                if (app->Cleanup()) {
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
        case WM_DISPLAYCHANGE:
            DebugLog("WindowProc: WM_DISPLAYCHANGE received. Re-creating icon.");
            app->CreateTrayIcon();
            app->UpdateTooltip();
            break;
        case WM_DPICHANGED:
            DebugLog("WindowProc: WM_DPICHANGED received. Refreshing tooltip.");
            app->UpdateTooltip();
            break;
        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}


void TaskTrayApp::MonitorDisplayChanges() {
    while (running) {
        Sleep(2000); // 2秒ごとに監視

        SharedMemoryHelper sharedMemoryHelper(this);
        std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
        if (gpuInfo.empty()) {
            DebugLog("MonitorDisplayChanges: GPU_INFO is empty! Skipping check.");
            continue;
        }

        std::string gpuVendorID, gpuDeviceID;
        size_t delimiter = gpuInfo.find(":");
        if (delimiter != std::string::npos) {
            gpuVendorID = gpuInfo.substr(0, delimiter);
            gpuDeviceID = gpuInfo.substr(delimiter + 1);
        }

        auto newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
        DebugLog("MonitorDisplayChanges: Retrieved " + std::to_string(newDisplays.size()) + " displays.");

        std::vector<std::string> oldDisplaySerials;
        HKEY hKeyRead;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, KEY_READ, &hKeyRead) == ERROR_SUCCESS) {
            wchar_t valueName[256];
            DWORD valueNameSize = 256;
            DWORD index = 0;
            while (RegEnumValue(hKeyRead, index, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                if (wcsncmp(valueName, L"SerialNumber", 12) == 0) {
                    wchar_t serialBuf[512];
                    DWORD serialBufSize = sizeof(serialBuf);
                    if (RegQueryValueEx(hKeyRead, valueName, NULL, NULL, (LPBYTE)serialBuf, &serialBufSize) == ERROR_SUCCESS) {
                        oldDisplaySerials.push_back(utf16_to_utf8(serialBuf));
                    }
                }
                valueNameSize = 256;
                index++;
            }
            RegCloseKey(hKeyRead);
        }

        std::vector<std::string> newDisplaySerials;
        for (const auto& display : newDisplays) {
            newDisplaySerials.push_back(display.serialNumber);
        }

        std::sort(oldDisplaySerials.begin(), oldDisplaySerials.end());
        std::sort(newDisplaySerials.begin(), newDisplaySerials.end());

        if (oldDisplaySerials != newDisplaySerials) {
            DebugLog("MonitorDisplayChanges: Display change detected.");

            sharedMemoryHelper.WriteSharedMemory("DISP_INFO_NUM", std::to_string(newDisplays.size()));
            for (size_t i = 0; i < newDisplays.size(); ++i) {
                std::string key = "DISP_INFO_" + std::to_string(i + 1);
                sharedMemoryHelper.WriteSharedMemory(key, newDisplays[i].serialNumber);
            }

            std::string selectedSerial = RegistryHelper::ReadSelectedDisplaySerial();
            bool selectionIsValid = false;
            if (!selectedSerial.empty()) {
                for (const auto& display : newDisplays) {
                    if (display.serialNumber == selectedSerial) {
                        selectionIsValid = true;
                        break;
                    }
                }
            }

            if (!selectionIsValid) {
                DebugLog("MonitorDisplayChanges: Selected monitor disconnected. Falling back to primary.");
                selectedSerial = "";
                for (const auto& display : newDisplays) {
                    if (display.isPrimary) {
                        selectedSerial = display.serialNumber;
                        break;
                    }
                }
                if (selectedSerial.empty() && !newDisplays.empty()) {
                    selectedSerial = newDisplays[0].serialNumber;
                }
            }

            if (!selectedSerial.empty()) {
                sharedMemoryHelper.WriteSharedMemory("DISP_INFO", selectedSerial);
                RegistryHelper::WriteSelectedDisplaySerial(selectedSerial);
                DebugLog("MonitorDisplayChanges: Final selected display: " + selectedSerial);
            }
            else {
                sharedMemoryHelper.WriteSharedMemory("DISP_INFO", "");
                RegistryHelper::WriteSelectedDisplaySerial("");
            }

            UpdateTooltip();

            HKEY hKeyWrite;
            if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, KEY_ALL_ACCESS, &hKeyWrite) == ERROR_SUCCESS) {
                std::vector<std::wstring> namesToDelete;
                wchar_t valueName[256];
                DWORD valueNameSize = 256;
                DWORD index = 0;
                while (RegEnumValue(hKeyWrite, index, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                    if (wcsncmp(valueName, L"SerialNumber", 12) == 0) {
                        namesToDelete.push_back(valueName);
                    }
                    valueNameSize = 256;
                    index++;
                }
                for (const auto& name : namesToDelete) {
                    RegDeleteValue(hKeyWrite, name.c_str());
                }
                RegCloseKey(hKeyWrite);
            }

            ::serialNumberIndex = 0;
            for (const auto& display : newDisplays) {
                RegistryHelper::WriteDISPInfoToRegistry(display.serialNumber);
            }
            DebugLog("MonitorDisplayChanges: Updated registry with new display list.");

            PostMessage(hwnd, WM_USER + 2, 0, 0);
        }
    }
}


void TaskTrayApp::RefreshDisplayList() {
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
    if (gpuInfo.empty()) {
        DebugLog("RefreshDisplayList: Failed to read GPU_INFO or GPU_INFO is empty.");
        MessageBox(hwnd, _T("メモリーから情報を読み取れません。製造元に問い合わせてください。アプリケーションを終了します。"), _T("エラー"), MB_OK | MB_ICONERROR);
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
    }

    std::vector<DisplayInfo> newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);

    if (newDisplays.empty()) {
        DebugLog("RefreshDisplayList: Failed to retrieve display list or no displays found.");
        sharedMemoryHelper.WriteSharedMemory("DISP_INFO_NUM", "0");
        sharedMemoryHelper.WriteSharedMemory("DISP_INFO", "");
    }
    else {
        sharedMemoryHelper.WriteSharedMemory("DISP_INFO_NUM", std::to_string(newDisplays.size()));
        DebugLog("RefreshDisplayList: Wrote DISP_INFO_NUM: " + std::to_string(newDisplays.size()));
        for (size_t i = 0; i < newDisplays.size(); ++i) {
            std::string key = "DISP_INFO_" + std::to_string(i + 1);
            sharedMemoryHelper.WriteSharedMemory(key, newDisplays[i].serialNumber);
        }

        std::string selectedSerial = RegistryHelper::ReadSelectedDisplaySerial();
        bool selectionIsValid = false;
        if (!selectedSerial.empty()) {
            for (const auto& display : newDisplays) {
                if (display.serialNumber == selectedSerial) {
                    selectionIsValid = true;
                    break;
                }
            }
        }

        if (!selectionIsValid) {
            DebugLog("RefreshDisplayList: No valid selection found in registry, or selected monitor disconnected. Falling back to primary.");
            selectedSerial = "";
            for (const auto& display : newDisplays) {
                if (display.isPrimary) {
                    selectedSerial = display.serialNumber;
                    break;
                }
            }
            if (selectedSerial.empty() && !newDisplays.empty()) {
                selectedSerial = newDisplays[0].serialNumber;
            }
        }

        if (!selectedSerial.empty()) {
            sharedMemoryHelper.WriteSharedMemory("DISP_INFO", selectedSerial);
            RegistryHelper::WriteSelectedDisplaySerial(selectedSerial);
            DebugLog("RefreshDisplayList: Final selected display (startup): " + selectedSerial);
        }
        else {
            DebugLog("RefreshDisplayList: No displays available to select.");
            sharedMemoryHelper.WriteSharedMemory("DISP_INFO", "");
            RegistryHelper::WriteSelectedDisplaySerial("");
        }
    }

    UpdateTooltip();

    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        std::vector<std::wstring> valueNames;
        wchar_t valueName[256];
        DWORD valueNameSize = 256;
        DWORD index = 0;
        while(RegEnumValue(hKey, index, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            if (wcsncmp(valueName, L"SerialNumber", 12) == 0) {
                valueNames.push_back(valueName);
            }
            valueNameSize = 256;
            index++;
        }

        for (const auto& name : valueNames) {
            RegDeleteValue(hKey, name.c_str());
        }

        RegCloseKey(hKey);
    }
    else {
        DebugLog("RefreshDisplayList: Failed to open registry key for clearing.");
    }

    ::serialNumberIndex = 0;
    for (const auto& display : newDisplays) {
        RegistryHelper::WriteDISPInfoToRegistry(display.serialNumber);
    }
    DebugLog("RefreshDisplayList: Updated registry with new display list.");
}

void TaskTrayApp::UpdateTooltip() {
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string selectedSerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    std::string tooltipText = "GPU & Display Manager";

    if (!selectedSerial.empty()) {
        std::string numDisplaysStr = sharedMemoryHelper.ReadSharedMemory("DISP_INFO_NUM");
        int numDisplays = 0;
        try {
            if (!numDisplaysStr.empty()) numDisplays = std::stoi(numDisplaysStr);
        }
        catch (...) {}

        for (int i = 1; i <= numDisplays; ++i) {
            std::string key = "DISP_INFO_" + std::to_string(i);
            if (sharedMemoryHelper.ReadSharedMemory(key) == selectedSerial) {
                tooltipText += " - Selected: Display " + std::to_string(i);
                break;
            }
        }
    }

    lstrcpyn(nid.szTip, utf8_to_utf16(tooltipText).c_str(), sizeof(nid.szTip) / sizeof(nid.szTip[0]));
    nid.uFlags |= NIF_TIP;

    if (!Shell_NotifyIcon(NIM_MODIFY, &nid)) {
        DebugLog("UpdateTooltip: Failed to modify tray icon tooltip.");
    }
    else {
        DebugLog("UpdateTooltip: Tooltip updated to: " + tooltipText);
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


