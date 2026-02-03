#include "SharedMemoryHelper.h"
#include "DebugLog.h"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>

const DWORD SHARED_MEMORY_SIZE = 256;

static std::atomic<uint64_t> g_lastOpenAttemptMs(0);

// Helper to convert std::string to std::wstring
std::wstring ConvertStringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// Helper to open mutex with rate limiting and error suppression
static HANDLE OpenMutexSafe(const std::wstring& mutexName, const std::string& debugName) {
    const uint64_t now = GetTickCount64();
    const uint64_t last = g_lastOpenAttemptMs.load(std::memory_order_relaxed);
    if (last != 0 && (now - last) < 500) {
        return NULL;
    }
    g_lastOpenAttemptMs.store(now, std::memory_order_relaxed);

    HANDLE hMutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
    if (!hMutex) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) {
            DebugLog("OpenMutex failed (" + debugName + ") err=" + std::to_string(err));
        }
    }
    return hMutex;
}

SharedMemoryHelper::SharedMemoryHelper() {}

bool SharedMemoryHelper::WriteSharedMemory(const std::string& name, const std::string& data) {
    const std::wstring wKey = ConvertStringToWString(name);
    const std::wstring mapName   = L"Global\\" + wKey;
    const std::wstring mutexName = L"Global\\" + wKey + L"_Mutex";
    const std::wstring eventName = L"Global\\" + wKey + L"_Event";

    // Try to open mutex using helper
    HANDLE hMutex = OpenMutexSafe(mutexName, name);
    if (!hMutex) {
        // Logging is handled in OpenMutexSafe (or suppressed)
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
    const std::wstring mapName   = L"Global\\" + wKey;
    const std::wstring mutexName = L"Global\\" + wKey + L"_Mutex";

    HANDLE hMutex = OpenMutexSafe(mutexName, name);
    bool locked = false;
    if (hMutex) {
        DWORD wr = WaitForSingleObject(hMutex, 2000);
        locked = (wr == WAIT_OBJECT_0 || wr == WAIT_ABANDONED);
    } else {
        // OpenMutexSafe suppressed err=2. Other errors logged.
        // If mutex is missing, we might be reading before service started.
        // We'll try to read anyway if the mapping exists (optimistic read),
        // but likely the mapping is also missing.
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
    std::wstring wEventName = L"Global\\" + ConvertStringToWString(name) + L"_Event";
    HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, wEventName.c_str());
    if (hEvent) {
        SetEvent(hEvent);
        CloseHandle(hEvent);
    } else {
         DebugLog("SignalEvent: Event not found (" + name + ").");
    }
}
