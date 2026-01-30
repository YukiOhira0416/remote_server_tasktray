#ifndef SHAREDMEMORYHELPER_H
#define SHAREDMEMORYHELPER_H

#include <string>
#include <windows.h>

class SharedMemoryHelper {
public:
    SharedMemoryHelper();

    // Writes data to an existing shared memory block. Returns false if the block does not exist.
    // Also signals the associated event if it exists.
    bool WriteSharedMemory(const std::string& name, const std::string& data);

    // Reads data from an existing shared memory block. Returns empty string if the block does not exist.
    std::string ReadSharedMemory(const std::string& name);

    // Signals an existing event.
    void SignalEvent(const std::string& name);
};

#endif // SHAREDMEMORYHELPER_H
