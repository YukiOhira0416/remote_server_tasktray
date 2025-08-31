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


static std::vector<std::string> ReadOrderedDisplaysFromSharedMemory(SharedMemoryHelper& shm) {
    std::vector<std::string> out;

    // Count
    std::string countStr = shm.ReadSharedMemory("DISP_INFO_COUNT");
    if (countStr.empty()) {
        return out;
    }

    int count = 0;
    try {
        count = std::stoi(countStr);
    } catch (...) {
        DebugLog("ReadOrderedDisplaysFromSharedMemory: Invalid COUNT value: " + countStr);
        return out;
    }

    for (int i = 0; i < count; ++i) {
        std::string key = "DISP_INFO_" + std::to_string(i);
        std::string val = shm.ReadSharedMemory(key);
        if (val.empty()) {
            DebugLog("ReadOrderedDisplaysFromSharedMemory: Missing " + key);
            // Stop at first missing to avoid partial lists
            break;
        }
        out.push_back(val);
    }

    return out;
}

static void UpdateTrayTooltip(HWND hwnd, NOTIFYICONDATA& nid, const std::string& primarySerial) {
    // Tooltip must exist before/after primary changes
    std::wstring tip = L"GPU & Display Manager\nPrimary: ";
    tip += std::wstring(primarySerial.begin(), primarySerial.end());

    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    nid.uFlags = NIF_TIP | NIF_MESSAGE | NIF_ICON;
    nid.hWnd = hwnd;
    Shell_NotifyIcon(NIM_MODIFY, &nid);
    DebugLog("UpdateTrayTooltip: Tooltip updated.");
}


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

        HMENU hMenu = CreatePopupMenu();
        if (hMenu == NULL) {
            DebugLog("Error: Failed to create popup menu.");
            return;
        }

        AppendMenu(hMenu, MF_STRING, 1, _T("終了"));

        HMENU hSubMenu = CreatePopupMenu();
        if (hSubMenu == NULL) {
            DebugLog("Error: Failed to create submenu.");
            DestroyMenu(hMenu);
            return;
        }

        // Build from shared memory (fallback to registry) in UpdateDisplayMenu
        UpdateDisplayMenu(hSubMenu, {});

        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, _T("ディスプレイの選択"));

        SetForegroundWindow(hwnd);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hMenu);
    }
    catch (const std::exception& e) {
        DebugLog(std::string("Exception: ") + e.what());
    }
}

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu, const std::vector<std::string> /*displays*/) {
    DebugLog("UpdateDisplayMenu: Start updating display menu.");

    // Clear existing items
    while (RemoveMenu(hMenu, 0, MF_BYPOSITION));

    SharedMemoryHelper sharedMemoryHelper(this);

    // Preferred source: shared memory ordered list
    std::vector<std::string> ordered = ReadOrderedDisplaysFromSharedMemory(sharedMemoryHelper);

    // Fallback: ordered registry read
    if (ordered.empty()) {
        ordered = RegistryHelper::ReadDISPInfoFromRegistryOrdered();
    }

    // Guard
    if (ordered.empty()) {
        AppendMenu(hMenu, MF_STRING | MF_DISABLED, 9999, _T("(No displays)"));
        DebugLog("UpdateDisplayMenu: No displays available.");
        return;
    }

    // Current selection (still stored under legacy "DISP_INFO")
    std::string selectedDisplaySerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    DebugLog("Selected display serial: " + selectedDisplaySerial);

    for (size_t i = 0; i < ordered.size(); ++i) {
        UINT flags = MF_STRING;
        if (ordered[i] == selectedDisplaySerial && !selectedDisplaySerial.empty()) {
            flags |= MF_CHECKED;
        }
        std::wstring displayName(ordered[i].begin(), ordered[i].end());
        if (AppendMenu(hMenu, flags, 100 + static_cast<UINT>(i), displayName.c_str())) {
            DebugLog("Successfully added menu item: " + ordered[i]);
        } else {
            DebugLog("Failed to add menu item: " + ordered[i]);
        }
    }

    DebugLog("UpdateDisplayMenu: Finished updating display menu.");
}

void TaskTrayApp::SelectDisplay(int displayIndex) {
    SharedMemoryHelper sharedMemoryHelper(this);

    // Preferred ordered list
    std::vector<std::string> DisplaysList = ReadOrderedDisplaysFromSharedMemory(sharedMemoryHelper);
    if (DisplaysList.empty()) {
        DisplaysList = RegistryHelper::ReadDISPInfoFromRegistryOrdered();
    }

    if (displayIndex < 0 || displayIndex >= static_cast<int>(DisplaysList.size())) {
        DebugLog("SelectDisplay: Invalid display index: " + std::to_string(displayIndex));
        return;
    }

    // Persist selection to legacy key
    if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO", DisplaysList[displayIndex])) {
        DebugLog("SelectDisplay: Failed to write display serial number to shared memory.");
        return;
    }
    DebugLog("SelectDisplay: Display serial number written to shared memory: " + DisplaysList[displayIndex]);

    // Update tooltip to reflect current primary (index 0 is primary in our scheme; selection may or may not be primary)
    std::string primary = DisplaysList.empty() ? std::string() : DisplaysList[0];
    UpdateTrayTooltip(hwnd, nid, primary);
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
    SharedMemoryHelper sharedMemoryHelper(this);
    std::vector<std::string> previousOrder;

    while (running) {
        Sleep(2000); // poll every 2s

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

        auto NewDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
        DebugLog("MonitorDisplayChanges: Retrieved new display list.");

        // Build ordered list of serials (primary first, then port order)
        std::vector<std::string> newOrder;
        newOrder.reserve(NewDisplays.size());
        for (const auto& d : NewDisplays) {
            if (!d.serialNumber.empty()) {
                newOrder.push_back(d.serialNumber);
            }
        }

        if (newOrder.empty()) {
            DebugLog("MonitorDisplayChanges: No serials in new order; skip.");
            continue;
        }

        // Detect change (order change or size change)
        bool changed = (newOrder.size() != previousOrder.size());
        if (!changed) {
            for (size_t i = 0; i < newOrder.size(); ++i) {
                if (newOrder[i] != previousOrder[i]) { changed = true; break; }
            }
        }

        if (changed) {
            previousOrder = newOrder;
            DebugLog("MonitorDisplayChanges: Order changed; updating shared memory and registry.");

            // Write shared memory ordered list
            if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO_COUNT", std::to_string(static_cast<int>(newOrder.size())))) {
                DebugLog("MonitorDisplayChanges: Failed to write DISP_INFO_COUNT.");
            }
            for (int i = 0; i < static_cast<int>(newOrder.size()); ++i) {
                std::string key = "DISP_INFO_" + std::to_string(i);
                if (!sharedMemoryHelper.WriteSharedMemory(key, newOrder[static_cast<size_t>(i)])) {
                    DebugLog("MonitorDisplayChanges: Failed to write " + key);
                }
            }
            // Legacy currently selected = primary[0]
            if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO", newOrder.front())) {
                DebugLog("MonitorDisplayChanges: Failed to write legacy DISP_INFO.");
            }

            // Rebuild registry values in order
            HKEY hKey;
            if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
                DWORD valueCount = 0;
                RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &valueCount, NULL, NULL, NULL, NULL);
                for (DWORD i = 0; i < valueCount; ++i) {
                    wchar_t valueName[256];
                    DWORD valueNameSize = static_cast<DWORD>(sizeof(valueName) / sizeof(valueName[0]));
                    if (RegEnumValue(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        RegDeleteValue(hKey, valueName);
                    }
                }
                RegCloseKey(hKey);
            } else {
                DebugLog("MonitorDisplayChanges: Failed to open registry key for clearing.");
            }

            ::serialNumberIndex = 0;
            char buffer[10];
            for (const auto& serial : newOrder) {
                _itoa_s(::serialNumberIndex, buffer, sizeof(buffer), 10);
                if (!RegistryHelper::WriteDISPInfoToRegistry(serial)) {
                    DebugLog("MonitorDisplayChanges: Failed to write SerialNumber" + std::string(buffer));
                }
            }

            // Refresh tooltip (primary = index 0)
            UpdateTrayTooltip(hwnd, nid, newOrder.front());

            // Notify UI thread to rebuild the menu
            PostMessage(hwnd, WM_USER + 2, 0, 0);
        }
    }
}


void TaskTrayApp::RefreshDisplayList() {
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string gpuInfo = sharedMemoryHelper.ReadSharedMemory("GPU_INFO");
    if (gpuInfo.empty()) {
        DebugLog("RefreshDisplayList: Failed to read GPU_INFO or GPU_INFO is empty.");
        int result = MessageBox(hwnd, _T("メモリーから情報を読み取れません。製造元に問い合わせてください。アプリケーションを終了します。"), _T("エラー"), MB_OK | MB_ICONERROR);
        if (result == IDOK) {
            PostQuitMessage(0);
        }
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

    // Ordered displays: primary first, then port order
    DebugLog("RefreshDisplayList: Calling GetDisplaysForGPU with VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    std::vector<DisplayInfo> newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
    if (newDisplays.empty()) {
        DebugLog("RefreshDisplayList: Failed to retrieve display list or no displays found.");
        return;
    }

    // Write ordered list to shared memory
    int count = static_cast<int>(newDisplays.size());
    if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO_COUNT", std::to_string(count))) {
        DebugLog("RefreshDisplayList: Failed to write DISP_INFO_COUNT.");
    }

    ::serialNumberIndex = 0;
    for (int i = 0; i < count; ++i) {
        const auto& display = newDisplays[static_cast<size_t>(i)];
        std::string key = "DISP_INFO_" + std::to_string(i);
        if (!sharedMemoryHelper.WriteSharedMemory(key, display.serialNumber)) {
            DebugLog("RefreshDisplayList: Failed to write " + key);
        }
        // Maintain legacy "selected" value as the primary (index 0)
        if (i == 0) {
            if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO", display.serialNumber)) {
                DebugLog("RefreshDisplayList: Failed to write legacy DISP_INFO.");
            } else {
                DebugLog("RefreshDisplayList: Wrote legacy DISP_INFO (primary).");
            }
        }
    }

    // Rewrite registry in the same order: SerialNumber0..N
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH_DISP, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        // Clear existing values
        DWORD valueCount = 0;
        RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &valueCount, NULL, NULL, NULL, NULL);
        for (DWORD i = 0; i < valueCount; ++i) {
            wchar_t valueName[256];
            DWORD valueNameSize = static_cast<DWORD>(sizeof(valueName) / sizeof(valueName[0]));
            if (RegEnumValue(hKey, 0, valueName, &valueNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                RegDeleteValue(hKey, valueName);
            }
        }
        RegCloseKey(hKey);
    } else {
        DebugLog("RefreshDisplayList: Failed to create or open registry key.");
    }

    char buffer[10];
    ::serialNumberIndex = 0;
    for (const auto& display : newDisplays) {
        _itoa_s(::serialNumberIndex, buffer, sizeof(buffer), 10);
        if (!RegistryHelper::WriteDISPInfoToRegistry(display.serialNumber)) {
            DebugLog("RefreshDisplayList: Failed to write SerialNumber" + std::string(buffer));
        } else {
            DebugLog("RefreshDisplayList: Wrote SerialNumber" + std::string(buffer));
        }
    }

    // Update tooltip (primary index 0)
    UpdateTrayTooltip(hwnd, nid, newDisplays[0].serialNumber);

    DebugLog("RefreshDisplayList: Completed.");
}

int TaskTrayApp::Run() {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}


