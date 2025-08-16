// SharedMemoryHelper.h
#ifndef SHAREDMEMORYHELPER_H
#define SHAREDMEMORYHELPER_H

#include <string>
#include <unordered_map>
#include <windows.h>
#include "TaskTrayApp.h"

class SharedMemoryHelper {
public:
    SharedMemoryHelper(TaskTrayApp* app);
    bool WriteSharedMemory(const std::string& name, const std::string& data);
    std::string ReadSharedMemory(const std::string& name);
    void SignalEvent(const std::string& name);
    bool DeleteSharedMemory(); // メソッドの宣言を追加
    bool DeleteEvent();

private:
    TaskTrayApp* app; // TaskTrayApp クラスのインスタンスを保持するメンバー変数
    SECURITY_ATTRIBUTES CreateSecurityAttributes(); // メンバー関数として宣言
};

#endif // SHAREDMEMORYHELPER_H
