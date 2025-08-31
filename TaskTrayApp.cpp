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
#include <regex>


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
            // *** DO NOT return here. Proceed to create a minimal menu. ***
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

        // Recommended by Microsoft so the menu reliably dismisses/focuses
        PostMessage(hwnd, WM_NULL, 0, 0);

        DestroyMenu(hMenu);
        DebugLog("Popup menu destroyed.");
    }
    catch (const std::exception& e) {
        DebugLog(std::string("Exception: ") + e.what());
    }
}

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu, const std::vector<std::string> displays) {
    DebugLog("UpdateDisplayMenu: Start updating display menu.");

    // 既存のメニュー項目を削除
    while (RemoveMenu(hMenu, 0, MF_BYPOSITION));

    // 現在選択されているディスプレイを取得
    SharedMemoryHelper sharedMemoryHelper(this);
    std::string selectedDisplaySerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    DebugLog("Selected display serial: " + selectedDisplaySerial);

    for (size_t i = 0; i < displays.size(); ++i) {
        UINT flags = MF_STRING;
        if (displays[i] == selectedDisplaySerial) {
            flags |= MF_CHECKED;
        }
        // Convert std::string to std::wstring
        std::wstring displayName(displays[i].begin(), displays[i].end());
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

        // Recompute primary adapter and refresh GPU_INFO if needed
        {
            auto gpus = GPUManager::GetInstalledGPUs();
            bool foundPrimaryAdapter = false;
            std::string newGpuInfo = "";

            DISPLAY_DEVICE dd;
            ZeroMemory(&dd, sizeof(dd));
            dd.cb = sizeof(dd);
            for (DWORD i = 0; EnumDisplayDevices(NULL, i, &dd, 0); ++i) {
                if ((dd.StateFlags & DISPLAY_DEVICE_ACTIVE) && (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) {
                    std::string deviceID = ConvertWStringToString(dd.DeviceID);
                    std::smatch m1, m2;
                    std::regex vendorRegex("VEN_([0-9A-Fa-f]+)");
                    std::regex deviceRegex("DEV_([0-9A-Fa-f]+)");
                    std::string venHex, devHex;
                    if (std::regex_search(deviceID, m1, vendorRegex) && m1.size() > 1) venHex = m1.str(1);
                    if (std::regex_search(deviceID, m2, deviceRegex) && m2.size() > 1) devHex = m2.str(1);
                    unsigned venDec = 0, devDec = 0;
                    std::stringstream ss;
                    ss << std::hex << venHex;
                    ss >> venDec;
                    ss.clear();
                    ss << std::hex << devHex;
                    ss >> devDec;

                    for (const auto& gpu : gpus) {
                        if ((unsigned)std::stoi(gpu.vendorID) == venDec &&
                            (unsigned)std::stoi(gpu.deviceID) == devDec) {
                            newGpuInfo = gpu.vendorID + ":" + gpu.deviceID;
                            foundPrimaryAdapter = true;
                            break;
                        }
                    }
                    break;
                }
            }

            if (foundPrimaryAdapter && !newGpuInfo.empty() && newGpuInfo != gpuInfo) {
                sharedMemoryHelper.WriteSharedMemory("GPU_INFO", newGpuInfo);
                DebugLog("MonitorDisplayChanges: Updated GPU_INFO to " + newGpuInfo);
                gpuInfo = newGpuInfo; // so subsequent code uses the latest
            }
        }

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
            // 変化があった場合、ディスプレイリストを更新
            Displays_Before = Displays_After;
            DebugLog("MonitorDisplayChanges: Display list updated.");

            // プライマリディスプレイのシリアルナンバーを共有メモリとレジストリに保存
            for (const auto& display : NewDisplays) {
                if (display.isPrimary) {
                    sharedMemoryHelper.WriteSharedMemory("DISP_INFO", display.serialNumber);
                    DebugLog("MonitorDisplayChanges: Primary display serial number saved: " + display.serialNumber);
                    break;
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
            }
            else {
                DebugLog("WriteDISPInfoToRegistry: Failed to create or open registry key.");
            }

            char buffer[10];
			::serialNumberIndex = 0;
            for (const auto& display : NewDisplays) {
                _itoa_s(::serialNumberIndex, buffer, sizeof(buffer), 10); // 変換を行う
                std::string displayInfo = std::string(buffer);

                if (!RegistryHelper::WriteDISPInfoToRegistry(display.serialNumber)) {
                    DebugLog("MonitorDisplayChanges: Failed to write primary display serial number to registry." + displayInfo);
                    continue;
                }

                DebugLog("MonitorDisplayChanges: Primary display serial number saved: " + displayInfo);

            }
            DebugLog("MonitorDisplayChanges: Retrieved display list.");

            // メインスレッドにメニューの更新を指示
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
    DebugLog("RefreshDisplayList: Calling GetDisplaysForGPU with VendorID: " + gpuVendorID + ", DeviceID: " + gpuDeviceID);
    std::vector<DisplayInfo> newDisplays = DisplayManager::GetDisplaysForGPU(gpuVendorID, gpuDeviceID);
    if (newDisplays.empty()) {
        DebugLog("RefreshDisplayList: Failed to retrieve display list or no displays found.");
    }
    else {
        bool primaryDisplayFound = false;
        for (const auto& display : newDisplays) {
            if (display.isPrimary) {
                if (!sharedMemoryHelper.WriteSharedMemory("DISP_INFO", display.serialNumber)) {
                    DebugLog("RefreshDisplayList: Failed to write primary display serial number to shared memory.");
                }
                else {
                    DebugLog("RefreshDisplayList: Primary display serial number writen sussesfully to shared memory.");
                }
                primaryDisplayFound = true;
                break;
            }

        }

        if (!primaryDisplayFound) {
            DebugLog("RefreshDisplayList: No primary display found.");
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


