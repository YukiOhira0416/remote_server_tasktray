#include "TaskTrayApp.h"
#include "StringConversion.h"
#include "GPUManager.h"
#include "DisplayManager.h"
#include "Utility.h"
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

// Returns vector of serials from shared memory in order DISP_INFO_0..N.
// If DISP_INFO_NUM is missing or invalid, returns empty vector.
static std::vector<std::string> ReadDisplayListFromSharedMemory(TaskTrayApp* app) {
    std::vector<std::string> out;
    if (!app) return out;
    SharedMemoryHelper shm(app);
    std::string numStr = shm.ReadSharedMemory("DISP_INFO_NUM");
    if (numStr.empty()) {
        DebugLog("ReadDisplayListFromSharedMemory: DISP_INFO_NUM not found.");
        return out;
    }
    // DISP_INFO_NUM is 0-based max index
    int maxIndex = -1;
    try {
        maxIndex = std::stoi(numStr);
    } catch (...) {
        DebugLog("ReadDisplayListFromSharedMemory: Invalid DISP_INFO_NUM value: " + numStr);
        return out;
    }
    if (maxIndex < 0) return out;
    for (int i = 0; i <= maxIndex; ++i) {
        std::string key = "DISP_INFO_" + std::to_string(i);
        std::string serial = shm.ReadSharedMemory(key.c_str());
        if (serial.empty()) {
            DebugLog("ReadDisplayListFromSharedMemory: Missing " + key);
            // We continue to preserve positions (robustness)
        }
        out.push_back(serial);
    }
    return out;
}

// Persists ordered display list to shared memory (DISP_INFO_0..N, DISP_INFO_NUM, and DISP_INFO for primary)
// and mirrors the same order to registry (SerialNumber0..N). Also resets and writes.
static void WriteDisplayListToSharedMemoryAndRegistry(TaskTrayApp* app,
                                                      const std::vector<DisplayInfo>& ordered) {
    if (!app) return;
    SharedMemoryHelper shm(app);

    // 1) Shared memory: clear old sequence by overwriting sequentially; callers must ensure count shrinks/grows safely.
    const int count = static_cast<int>(ordered.size());
    const int maxIndex = (count == 0) ? -1 : (count - 1);

    // DISP_INFO_NUM is zero-based max index
    if (!shm.WriteSharedMemory("DISP_INFO_NUM", std::to_string(maxIndex))) {
        DebugLog("WriteDisplayListToSharedMemoryAndRegistry: Failed to write DISP_INFO_NUM.");
    }

    for (int i = 0; i < count; ++i) {
        const std::string key = "DISP_INFO_" + std::to_string(i);
        const std::string& serial = ordered[i].serialNumber;
        if (!shm.WriteSharedMemory(key.c_str(), serial)) {
            DebugLog("WriteDisplayListToSharedMemoryAndRegistry: Failed to write " + key);
        }
    }

    // Also maintain single "DISP_INFO" as the selected/primary by default.
    if (count > 0) {
        const std::string& primarySerial = ordered[0].serialNumber;
        if (!shm.WriteSharedMemory("DISP_INFO", primarySerial)) {
            DebugLog("WriteDisplayListToSharedMemoryAndRegistry: Failed to write DISP_INFO (primary).");
        }
    }

    // 2) Registry mirror: clear and rewrite SerialNumberN in the same order.
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        // Delete existing values before writing new ones.
        // This is tricky because deleting a value can shift the indices of the remaining values.
        // The robust way is to query the value name at index 0 repeatedly until the function fails.
        wchar_t valueName[256];
        DWORD valueNameSize = static_cast<DWORD>(std::size(valueName));
        while (RegEnumValue(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            if (RegDeleteValue(hKey, valueName) != ERROR_SUCCESS) {
                // If deletion fails, break to avoid an infinite loop.
                DebugLog("WriteDisplayListToSharedMemoryAndRegistry: Failed to delete registry value: " + utf16_to_utf8(valueName));
                break;
            }
            // Reset size for the next call
            valueNameSize = static_cast<DWORD>(std::size(valueName));
        }
        RegCloseKey(hKey);
    } else {
        DebugLog("WriteDisplayListToSharedMemoryAndRegistry: Failed to open registry for cleanup.");
    }

    // Reset global index and write each serial via helper to preserve existing logging/locking.
    ::serialNumberIndex = 0;  // uses Globals.cpp global
    for (const auto& di : ordered) {
        if (!RegistryHelper::WriteDISPInfoToRegistry(di.serialNumber)) {
            DebugLog("WriteDisplayListToSharedMemoryAndRegistry: Failed to write serial to registry.");
        }
    }
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

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu, const std::vector<std::string> /*unused*/) {
    DebugLog("UpdateDisplayMenu: Start updating display menu.");

    while (RemoveMenu(hMenu, 0, MF_BYPOSITION));

    SharedMemoryHelper sharedMemoryHelper(this);
    std::string selectedDisplaySerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    DebugLog("Selected display serial: " + selectedDisplaySerial);

    std::vector<std::string> displays = ReadDisplayListFromSharedMemory(this);
    if (displays.empty()) {
        DebugLog("UpdateDisplayMenu: No displays found in shared memory.");
    }

    for (size_t i = 0; i < displays.size(); ++i) {
        UINT flags = MF_STRING;
        if (!displays[i].empty() && displays[i] == selectedDisplaySerial) {
            flags |= MF_CHECKED;
        }
        std::wstring displayName(displays[i].begin(), displays[i].end());
        if (AppendMenu(hMenu, flags, 100 + (UINT)i, displayName.c_str())) {
            DebugLog("Successfully added menu item: " + displays[i]);
        } else {
            DebugLog("Failed to add menu item: " + displays[i]);
        }
    }
    DebugLog("UpdateDisplayMenu: Finished updating display menu.");
}

void TaskTrayApp::SelectDisplay(int displayIndex) {
    std::vector<std::string> DisplaysList = ReadDisplayListFromSharedMemory(this);
    if (displayIndex < 0 || displayIndex >= static_cast<int>(DisplaysList.size())) {
        DebugLog("SelectDisplay: Invalid display index: " + std::to_string(displayIndex));
        return;
    }

    SharedMemoryHelper sharedMemoryHelper(this);
    const std::string& chosen = DisplaysList[displayIndex];

    if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO", chosen)) {
        DebugLog("SelectDisplay: Failed to write selected display to shared memory.");
        return;
    }
    DebugLog("SelectDisplay: Selected display written to shared memory: " + chosen);

    if (!RegistryHelper::WriteSelectedDisplay(chosen)) {
        DebugLog("SelectDisplay: Failed to persist selected display to registry.");
        // do not return; menu update should still proceed
    }

    // Update (rebuild) menu state (checkmarks) for subsequent opens
    HMENU hMenu = CreatePopupMenu();
    if (hMenu == NULL) {
        DebugLog("SelectDisplay: Failed to create popup menu.");
        return;
    }
    UpdateDisplayMenu(hMenu, DisplaysList);
    DestroyMenu(hMenu);
    DebugLog("SelectDisplay: Display menu updated after selection.");
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
        case WM_DPICHANGED: {
            DebugLog("WindowProc: WM_DPICHANGED received.");
            // If you had a resizable/visible window, you would resize to *lParam suggested rect.
            // For the hidden tray host window, nothing else is strictly required,
            // but handling this ensures future UI scales properly.
            return 0;
        }
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
            //DebugLog("WindowProc: Default case - Message received: " + std::to_string(uMsg));
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
            continue;
        }

        auto newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
        DebugLog("MonitorDisplayChanges: Retrieved new display list.");

        // Reorder: primary first, others in original order
        std::stable_sort(newDisplays.begin(), newDisplays.end(),
            [](const DisplayInfo& a, const DisplayInfo& b) {
                if (a.isPrimary != b.isPrimary) return a.isPrimary && !b.isPrimary;
                return false;
            });

        // Compare with the current state in shared memory to detect changes
        std::vector<std::string> displaysBefore = ReadDisplayListFromSharedMemory(this);
        std::vector<std::string> displaysAfter;
        for (const auto& display : newDisplays) {
            if (!display.serialNumber.empty()) {
                displaysAfter.push_back(display.serialNumber);
            }
        }

        if (displaysBefore != displaysAfter) {
            DebugLog("MonitorDisplayChanges: Display list has changed. Updating...");
            WriteDisplayListToSharedMemoryAndRegistry(this, newDisplays);
            DebugLog("MonitorDisplayChanges: Wrote new display list to shared memory and registry.");

            // Notify the main thread to update the menu
            PostMessage(hwnd, WM_USER + 2, 0, 0);
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
    }

    // Reorder: primary first, others in original order
    std::stable_sort(newDisplays.begin(), newDisplays.end(),
        [](const DisplayInfo& a, const DisplayInfo& b) {
            if (a.isPrimary != b.isPrimary) return a.isPrimary && !b.isPrimary; // primary first
            return false; // keep original order for non-primary
        });

    WriteDisplayListToSharedMemoryAndRegistry(this, newDisplays);
    DebugLog("RefreshDisplayList: Wrote new display list to shared memory and registry.");
}

int TaskTrayApp::Run() {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}


