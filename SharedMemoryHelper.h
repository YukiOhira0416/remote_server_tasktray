// SharedMemoryHelper.h
#ifndef SHAREDMEMORYHELPER_H
#define SHAREDMEMORYHELPER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <windows.h>
#include "TaskTrayApp.h"

class SharedMemoryHelper {
public:
    explicit SharedMemoryHelper(TaskTrayApp* app);
    bool WriteSharedMemory(const std::string& name, const std::string& data);
    std::string ReadSharedMemory(const std::string& name);
    bool WriteDisplayList(const std::vector<std::string>& serials);   // writes DISP_INFO_0..N
    std::vector<std::string> ReadDisplayList();                       // reads DISP_INFO_0..N (contiguous from 0)
    void SignalEvent(const std::string& name);
    bool DeleteSharedMemory(); // メソッドの宣言を追加
    bool DeleteEvent();

private:
    TaskTrayApp* app; // TaskTrayApp クラスのインスタンスを保持するメンバー変数
    SECURITY_ATTRIBUTES CreateSecurityAttributes(); // メンバー関数として宣言
};

#endif // SHAREDMEMORYHELPER_H
