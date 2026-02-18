#include "SharedMemoryHelper.h"
#include "DebugLog.h"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>

const DWORD SHARED_MEMORY_SIZE = 256;

// Helper to convert std::string to std::wstring
std::wstring ConvertStringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

SharedMemoryHelper::SharedMemoryHelper() {}

bool SharedMemoryHelper::WriteSharedMemory(const std::string& name, const std::string& data) {
    const std::wstring wKey = ConvertStringToWString(name);

    // We support both Global\ and Local\ namespaces.
    // Try Global first (service-created objects), then Local as a fallback for
    // same-session scenarios.
    const wchar_t* kNamespaces[]     = { L"Global\\", L"Local\\" };
    const char*    kNamespaceNames[] = { "Global",    "Local"    };

    std::wstring chosenPrefix;
    HANDLE       hMutex  = nullptr;
    DWORD        lastErr = 0;

    // Try to open mutex in Global then Local.
    for (int i = 0; i < 2 && !hMutex; ++i) {
        std::wstring mutexName = std::wstring(kNamespaces[i]) + wKey + L"_Mutex";
        hMutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
        if (!hMutex) {
            lastErr = GetLastError();
            if (lastErr != ERROR_FILE_NOT_FOUND) {
                DebugLog(
                    std::string("WriteSharedMemory: OpenMutex failed (") + name +
                    ") ns=" + kNamespaceNames[i] +
                    " err=" + std::to_string(lastErr));
                // For non-FNF errors we bail out immediately.
                return false;
            }
        } else {
            chosenPrefix = kNamespaces[i];
        }
    }

    if (!hMutex) {
        DebugLog(
            std::string("WriteSharedMemory: OpenMutex failed (") + name +
            ") in both Global and Local namespaces. lastErr=" + std::to_string(lastErr));
        return false;
    }

    DWORD waitResult = WaitForSingleObject(hMutex, 2000);
    bool locked = (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED);

    if (waitResult == WAIT_ABANDONED) {
        DebugLog("WriteSharedMemory: Mutex abandoned (" + name + "). Proceeding.");
    } else if (waitResult == WAIT_TIMEOUT) {
        DebugLog("WriteSharedMemory: Mutex timeout (" + name + "). Proceeding without lock.");
        // We can choose to fail here, but best-effort write is usually better for UI responsiveness.
    }

    std::wstring mapName   = chosenPrefix + wKey;
    std::wstring eventName = chosenPrefix + wKey + L"_Event";

    // Try to open file mapping.
    HANDLE hMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, mapName.c_str());
    if (!hMap) {
        DebugLog("WriteSharedMemory: Shared memory not found: " + name);
        if (locked) ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return false;
    }

    void* p = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, SHARED_MEMORY_SIZE);
    if (!p) {
        DebugLog("WriteSharedMemory: MapViewOfFile failed.");
        CloseHandle(hMap);
        if (locked) ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return false;
    }

    // Zero out and copy data
    memset(p, 0, SHARED_MEMORY_SIZE);
    size_t copySize = std::min<size_t>(data.size(), SHARED_MEMORY_SIZE - 1);
    memcpy(p, data.c_str(), copySize);
    ((char*)p)[copySize] = '\0'; // Ensure null termination

    UnmapViewOfFile(p);
    CloseHandle(hMap);

    // Try to open and signal event
    HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (hEvent) {
        SetEvent(hEvent);
        CloseHandle(hEvent);
    } else {
        // Event might not be created by service yet, or not needed for this key.
        // We don't fail the write operation just because the event is missing,
        // unless the protocol strictly requires it.
        DebugLog("WriteSharedMemory: Event not found (" + name + ").");
    }

    if (locked) ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return true;
}

std::string SharedMemoryHelper::ReadSharedMemory(const std::string& name) {
    const std::wstring wKey = ConvertStringToWString(name);

    const wchar_t* kNamespaces[]     = { L"Global\\", L"Local\\" };
    const char*    kNamespaceNames[] = { "Global",    "Local"    };

    HANDLE       hMutex        = nullptr;
    bool         locked        = false;
    std::wstring chosenPrefix;
    DWORD        lastErr       = 0;

    // Try to open mutex in Global then Local.
    for (int i = 0; i < 2 && !hMutex; ++i) {
        std::wstring mutexName = std::wstring(kNamespaces[i]) + wKey + L"_Mutex";
        hMutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
        if (!hMutex) {
            lastErr = GetLastError();
            if (lastErr != ERROR_FILE_NOT_FOUND) {
                // AccessDenied(5) is possible, so don't misdiagnose, but we also
                // do not want to spam logs on every tick if the service is down.
                DebugLog(
                    std::string("ReadSharedMemory: OpenMutex failed (") + name +
                    ") ns=" + kNamespaceNames[i] +
                    " err=" + std::to_string(lastErr));
                // In this case treat as hard failure.
                return "";
            }
        } else {
            chosenPrefix = kNamespaces[i];
        }
    }

    if (hMutex) {
        DWORD wr = WaitForSingleObject(hMutex, 2000);
        locked = (wr == WAIT_OBJECT_0 || wr == WAIT_ABANDONED);
    } else {
        // If mutex is missing in both namespaces, we might be reading before
        // the service started. We'll try to read anyway if the mapping exists
        // (optimistic read), but likely the mapping is also missing.
    }

    std::wstring mapName;
    if (!chosenPrefix.empty()) {
        mapName = chosenPrefix + wKey;
    } else {
        // Preserve existing behavior: if we could not find any mutex, fall back
        // to Global\ only as a best-effort read.
        mapName = std::wstring(L"Global\\") + wKey;
    }

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, mapName.c_str());
    if (!hMap) {
        // Quietly fail for DISP_INFO_NUM checks to avoid log spam on every tick if service is down.
        // Or log once. For now, we return empty string.
        if (hMutex) { if (locked) ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return "";
    }

    void* p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, SHARED_MEMORY_SIZE);
    if (!p) {
        CloseHandle(hMap);
        if (hMutex) { if (locked) ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return "";
    }

    std::string s((char*)p);
    UnmapViewOfFile(p);
    CloseHandle(hMap);

    if (hMutex) { if (locked) ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return s;
}

void SharedMemoryHelper::SignalEvent(const std::string& name) {
    const std::wstring wKey = ConvertStringToWString(name);

    const wchar_t* kNamespaces[]     = { L"Global\\", L"Local\\" };
    const char*    kNamespaceNames[] = { "Global",    "Local"    };

    for (int i = 0; i < 2; ++i) {
        std::wstring wEventName = std::wstring(kNamespaces[i]) + wKey + L"_Event";
        HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, wEventName.c_str());
        if (hEvent) {
            SetEvent(hEvent);
            CloseHandle(hEvent);
            return;
        } else {
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND) {
                DebugLog(
                    std::string("SignalEvent: Event not found (") + name +
                    ") ns=" + kNamespaceNames[i] +
                    " err=" + std::to_string(err));
                // For non-FNF we stop trying other namespaces, as this likely indicates a real error.
                return;
            }
        }
    }

    DebugLog("SignalEvent: Event not found (" + name + ") in both Global and Local namespaces.");
}
