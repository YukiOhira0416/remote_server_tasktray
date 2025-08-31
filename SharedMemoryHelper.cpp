#include "SharedMemoryHelper.h"
#include "DebugLog.h"
#include <windows.h>
#include <functional>
#include <algorithm>
#include <string>
#include <iostream>
#include <cstring>
#include <sddl.h>
#include <tchar.h>
#include <unordered_map>
#include "TaskTrayApp.h"
#include <mutex>

const DWORD SHARED_MEMORY_SIZE = 256;
std::unordered_map<std::string, HANDLE> eventHandleMap;
HANDLE hMapFile_WriteSharedMemory;
HANDLE hEvent;
std::mutex sharedMemoryMutex;

// SharedMemoryHelper クラスのコンストラクタ
SharedMemoryHelper::SharedMemoryHelper(TaskTrayApp* app) : app(app) {}

// std::string を std::wstring に変換する関数
std::wstring ConvertStringToWString(const std::string& str) {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// 現在のユーザーのSIDを取得する関数
std::string GetCurrentUserSID() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DebugLog("GetCurrentUserSID: OpenProcessToken failed with error: " + std::to_string(GetLastError()));
        return "";
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        DebugLog("GetCurrentUserSID: GetTokenInformation failed with error: " + std::to_string(GetLastError()));
        CloseHandle(token);
        return "";
    }

    CloseHandle(token);

    PSID sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    LPSTR sidString;
    if (!ConvertSidToStringSidA(sid, &sidString)) {
        DebugLog("GetCurrentUserSID: ConvertSidToStringSidA failed with error: " + std::to_string(GetLastError()));
        return "";
    }

    std::string sidStr(sidString);
    LocalFree(sidString);
    return sidStr;
}

// CreateSecurityAttributes 関数を SharedMemoryHelper クラスのメンバー関数として実装
SECURITY_ATTRIBUTES SharedMemoryHelper::CreateSecurityAttributes() {
    std::string userSID = GetCurrentUserSID();
    if (userSID.empty()) {
        DebugLog("CreateSecurityAttributes: Failed to get current user SID.");
        return {};
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = nullptr;

    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + ConvertStringToWString(userSID) + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl.c_str(),
        SDDL_REVISION_1,
        &sa.lpSecurityDescriptor,
        NULL)) {
        DWORD error = GetLastError();
        DebugLog("CreateSecurityAttributes: ConvertStringSecurityDescriptorToSecurityDescriptorW failed with error: " + std::to_string(error));
        sa.lpSecurityDescriptor = nullptr;
    }
    else {
        DebugLog("CreateSecurityAttributes: ConvertStringSecurityDescriptorToSecurityDescriptorW succeeded.");
    }

    return sa;
}

bool SharedMemoryHelper::WriteSharedMemory(const std::string& name, const std::string& data) {
    std::lock_guard<std::mutex> lock(sharedMemoryMutex);

    std::wstring fullName = L"Local\\" + ConvertStringToWString(name);
    std::wstring mutexName = L"Local\\Mutex_" + ConvertStringToWString(name);
    HANDLE hMutex = CreateMutexW(NULL, FALSE, mutexName.c_str());
    if (hMutex == NULL) {
        DebugLog("WriteSharedMemory: Failed to create mutex with error: " + std::to_string(GetLastError()));
        return false;
    }
    DebugLog("WriteSharedMemory: Waiting for mutex.");
    WaitForSingleObject(hMutex, INFINITE);
    DebugLog("WriteSharedMemory: Mutex acquired.");

    SECURITY_ATTRIBUTES sa = CreateSecurityAttributes();
    if (sa.lpSecurityDescriptor == nullptr) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return false;
    }

    hMapFile_WriteSharedMemory = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, SHARED_MEMORY_SIZE, fullName.c_str());
    if (hMapFile_WriteSharedMemory == NULL) {
        DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS) {
            hMapFile_WriteSharedMemory = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, fullName.c_str());
            if (hMapFile_WriteSharedMemory == NULL) {
                DebugLog("WriteSharedMemory: OpenFileMappingW failed with error: " + std::to_string(GetLastError()));
                LocalFree(sa.lpSecurityDescriptor);
                ReleaseMutex(hMutex);
                CloseHandle(hMutex);
                return false;
            }
        }
        else {
            DebugLog("WriteSharedMemory: CreateFileMappingW failed with error: " + std::to_string(error));
            LocalFree(sa.lpSecurityDescriptor);
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
            return false;
        }
    }
    else {
        DebugLog("WriteSharedMemory: CreateFileMappingW succeeded.");
    }

    LPVOID pBuf = MapViewOfFile(hMapFile_WriteSharedMemory, FILE_MAP_WRITE, 0, 0, SHARED_MEMORY_SIZE);
    if (pBuf == NULL) {
        DWORD error = GetLastError();
        DebugLog("WriteSharedMemory: MapViewOfFile failed with error: " + std::to_string(error));
        CloseHandle(hMapFile_WriteSharedMemory);
        LocalFree(sa.lpSecurityDescriptor);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return false;
    }

    memset(pBuf, 0, SHARED_MEMORY_SIZE);
    memcpy(pBuf, data.c_str(), data.size() + 1);

    DebugLog("WriteSharedMemory: Data written to shared memory: " + data);

    UnmapViewOfFile(pBuf);
    SignalEvent(name);

    if (sa.lpSecurityDescriptor) {
        LocalFree(sa.lpSecurityDescriptor);
    }

    ReleaseMutex(hMutex);
    DebugLog("WriteSharedMemory: Mutex released.");
    CloseHandle(hMutex);
    return true;
}

std::string SharedMemoryHelper::ReadSharedMemory(const std::string& name) {
    std::lock_guard<std::mutex> lock(sharedMemoryMutex);

    std::wstring mutexName = L"Local\\Mutex_" + ConvertStringToWString(name);
    HANDLE hMutex = CreateMutexW(NULL, FALSE, mutexName.c_str());
    if (hMutex == NULL) {
        DebugLog("ReadSharedMemory: Failed to create mutex with error: " + std::to_string(GetLastError()));
        return "";
    }
    DebugLog("ReadSharedMemory: Waiting for mutex.");
    WaitForSingleObject(hMutex, INFINITE);
    DebugLog("ReadSharedMemory: Mutex acquired.");

    std::wstring eventName = L"Local\\" + ConvertStringToWString(name) + L"_Event";
    HANDLE hEventFile = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName.c_str());
    if (hEventFile == NULL) {
        DWORD error = GetLastError();
        DebugLog("ReadSharedMemory: OpenEventW failed with error: " + std::to_string(error) + " (" + std::system_category().message(error) + ")");
        if (error == ERROR_FILE_NOT_FOUND) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
            return "";
        }
        else {
            return "";
        }
    }
    else {
        DebugLog("ReadSharedMemory: OpenEventW succeeded.");
    }
    DebugLog("ReadSharedMemory: Waiting for event handle: " + std::to_string(reinterpret_cast<uintptr_t>(hEventFile)));

    DWORD dwWaitResult = WaitForSingleObject(hEventFile, 5000);
    std::string data;
    if (dwWaitResult == WAIT_OBJECT_0) {
        std::wstring sharedMemoryName = L"Local\\" + ConvertStringToWString(name);
        HANDLE hMapFile = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, sharedMemoryName.c_str());
        if (hMapFile == NULL) {
            DebugLog("ReadSharedMemory: OpenFileMappingW failed with error: " + std::to_string(GetLastError()));
            CloseHandle(hEventFile);
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
            return "";
        }

        LPVOID pBuf = MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, SHARED_MEMORY_SIZE);
        if (pBuf == NULL) {
            DebugLog("ReadSharedMemory: MapViewOfFile failed with error: " + std::to_string(GetLastError()));
            CloseHandle(hMapFile);
            CloseHandle(hEventFile);
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
            return "";
        }

        data = std::string(static_cast<char*>(pBuf));

        if (data.empty()) {
            DebugLog("ReadSharedMemory: Error - returned empty data for " + name);
        }

        UnmapViewOfFile(pBuf);
        CloseHandle(hMapFile);

        ReleaseMutex(hMutex);
        DebugLog("ReadSharedMemory: Mutex released.");
        CloseHandle(hMutex);

        return data;
    }
    else if (dwWaitResult == WAIT_TIMEOUT) {
        DebugLog("ReadSharedMemory: WaitForSingleObject timed out.");
    }
    else {
        DebugLog("ReadSharedMemory: WaitForSingleObject failed with error: " + std::to_string(GetLastError()));
    }

    CloseHandle(hEventFile);

    ReleaseMutex(hMutex);
    DebugLog("ReadSharedMemory: Mutex released.");
    CloseHandle(hMutex);

    return data;
}

bool SharedMemoryHelper::TryReadSharedMemoryNoWait(const std::string& name, std::string& out) {
    // NOTE: Non-blocking path for UI thread. Do NOT wait on events here.
    std::wstring mutexName = L"Local\\Mutex_" + ConvertStringToWString(name);
    HANDLE hMutex = CreateMutexW(NULL, FALSE, mutexName.c_str());
    if (hMutex == NULL) {
        DebugLog("TryReadSharedMemoryNoWait: CreateMutexW failed with error: " + std::to_string(GetLastError()));
        return false;
    }

    // Small timeout to avoid UI stalls.
    DWORD wait = WaitForSingleObject(hMutex, 20); // 20ms max
    if (wait != WAIT_OBJECT_0) {
        DebugLog("TryReadSharedMemoryNoWait: Mutex not acquired quickly (wait=" + std::to_string(wait) + ").");
        CloseHandle(hMutex);
        return false;
    }

    bool ok = false;
    std::wstring sharedMemoryName = L"Local\\" + ConvertStringToWString(name);
    HANDLE hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemoryName.c_str());
    if (hMapFile != NULL) {
        LPVOID pBuf = MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, SHARED_MEMORY_SIZE);
        if (pBuf != NULL) {
            const char* p = static_cast<const char*>(pBuf);
            out.assign(p, strnlen(p, SHARED_MEMORY_SIZE));
            UnmapViewOfFile(pBuf);
            ok = !out.empty();
        } else {
            DebugLog("TryReadSharedMemoryNoWait: MapViewOfFile failed with error: " + std::to_string(GetLastError()));
        }
        CloseHandle(hMapFile);
    } else {
        // Mapping may not exist yet; this is okay for UI path.
        DebugLog("TryReadSharedMemoryNoWait: OpenFileMappingW returned NULL (not created yet).");
    }

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return ok;
}

void SharedMemoryHelper::SignalEvent(const std::string& name) {
    std::wstring wEventName = L"Local\\" + ConvertStringToWString(name) + L"_Event";

    SECURITY_ATTRIBUTES sa = CreateSecurityAttributes();
    if (sa.lpSecurityDescriptor == nullptr) {
        DebugLog("SignalEvent: Failed to create security attributes.");
        return;
    }

    hEvent = nullptr;
    auto it = eventHandleMap.find(name);
    if (it != eventHandleMap.end()) {
        hEvent = it->second;
    }
    else {
        hEvent = CreateEventW(&sa, TRUE, FALSE, wEventName.c_str());
        if (hEvent == NULL) {
            DWORD error = GetLastError();
            DebugLog("SignalEvent: CreateEventW failed with error: " + std::to_string(error) + " (" + std::system_category().message(error) + ")");
            if (sa.lpSecurityDescriptor) {
                LocalFree(sa.lpSecurityDescriptor);
            }
            return;
        }
        else {
            DebugLog("SignalEvent: CreateEventW succeeded.");
        }
        eventHandleMap[name] = hEvent;
    }

    if (!SetEvent(hEvent)) {
        DWORD error = GetLastError();
        DebugLog("SignalEvent: SetEvent failed with error: " + std::to_string(error) + " (" + std::system_category().message(error) + ")");
    }
    else {
        DebugLog("SignalEvent: SetEvent succeeded.");
    }

    if (sa.lpSecurityDescriptor) {
        LocalFree(sa.lpSecurityDescriptor);
    }
}

bool SharedMemoryHelper::DeleteSharedMemory() {
    CloseHandle(hMapFile_WriteSharedMemory);
    hMapFile_WriteSharedMemory = nullptr;
    return true;
}

bool SharedMemoryHelper::DeleteEvent() {
    CloseHandle(hEvent);
    hEvent = nullptr;
    return true;
}
