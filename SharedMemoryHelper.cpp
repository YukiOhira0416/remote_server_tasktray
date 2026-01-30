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

// SharedMemoryHelper クラスのコンストラクタ
SharedMemoryHelper::SharedMemoryHelper(TaskTrayApp* app) : app(app) {}

// std::string を std::wstring に変換する関数
std::wstring ConvertStringToWString(const std::string& str) {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

bool SharedMemoryHelper::WriteSharedMemory(const std::string& name, const std::string& data) {
    // Open-only: mutexもmappingもeventも "Open" しかしない
    const std::wstring wKey = ConvertStringToWString(name);
    const std::wstring mapName   = L"Global\\" + wKey;
    const std::wstring mutexName = L"Global\\" + wKey + L"_Mutex";
    const std::wstring eventName = L"Global\\" + wKey + L"_Event";

    HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, mutexName.c_str());
    if (hMutex) {
        WaitForSingleObject(hMutex, 2000); // 開けない場合は排他なしで続行（best effort）
    } else {
        DebugLog("WriteSharedMemory: Mutex not found (" + name + "), proceeding without lock.");
    }

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapName.c_str());
    if (!hMap) {
        DebugLog("WriteSharedMemory: Shared memory not found: " + name);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return false; // 無ければ失敗（後で再試行）
    }

    void* p = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, SHARED_MEMORY_SIZE);
    if (!p) {
        DebugLog("WriteSharedMemory: MapViewOfFile failed.");
        CloseHandle(hMap);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return false;
    }

    memset(p, 0, SHARED_MEMORY_SIZE);
    // data.size() can be larger than SHARED_MEMORY_SIZE.
    size_t copySize = std::min<size_t>(data.size(), SHARED_MEMORY_SIZE - 1);
    memcpy(p, data.c_str(), copySize);
    ((char*)p)[copySize] = '\0'; // Ensure null termination

    UnmapViewOfFile(p);
    CloseHandle(hMap);

    // eventもOpenしてSetするだけ（Create禁止）
    HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (hEvent) {
        SetEvent(hEvent);
        CloseHandle(hEvent);
    } else {
        DebugLog("WriteSharedMemory: Event not found (" + name + ").");
    }

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return true;
}

std::string SharedMemoryHelper::ReadSharedMemory(const std::string& name) {
    const std::wstring wKey = ConvertStringToWString(name);
    const std::wstring mapName   = L"Global\\" + wKey;
    const std::wstring mutexName = L"Global\\" + wKey + L"_Mutex";

    HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, mutexName.c_str());
    if (hMutex) {
        WaitForSingleObject(hMutex, 2000);
    } else {
         DebugLog("ReadSharedMemory: Mutex not found (" + name + "), proceeding without lock.");
    }

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, mapName.c_str());
    if (!hMap) {
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return ""; // 無ければ空（後で再試行）
    }
    void* p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, SHARED_MEMORY_SIZE);
    if (!p) {
        CloseHandle(hMap);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return "";
    }
    std::string s((char*)p);
    UnmapViewOfFile(p);
    CloseHandle(hMap);
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return s;
}

void SharedMemoryHelper::SignalEvent(const std::string& name) {
    std::wstring wEventName = L"Global\\" + ConvertStringToWString(name) + L"_Event";
    HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, wEventName.c_str());
    if (hEvent) {
        SetEvent(hEvent);
        CloseHandle(hEvent);
    } else {
         DebugLog("SignalEvent: Event not found (" + name + ").");
    }
}

bool SharedMemoryHelper::DeleteSharedMemory() {
    // Client side doesn't own shared memory anymore.
    return true;
}

bool SharedMemoryHelper::DeleteEvent() {
    // Client side doesn't own event anymore.
    return true;
}
