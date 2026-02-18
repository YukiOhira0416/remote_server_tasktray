#ifndef MODE_SYNC_SERVER_H
#define MODE_SYNC_SERVER_H

#include <thread>
#include <atomic>
#include <mutex>
#include <string>

class TaskTrayApp;

// TCP server running in the task tray process to synchronize the
// "Mode Selection" (Low/Medium/High speed) between the task tray UI
// and remote Qt client(s).
//
// Protocol (line based, UTF-8, '\n' terminated):
//   Client -> Server:
//     MODE <n>      # 1=Low-speed, 2=Medium-speed, 3=High-speed
//
//   Server -> Client:
//     MODE <n>      # current mode (1/2/3)
//
class ModeSyncServer
{
public:
    explicit ModeSyncServer(TaskTrayApp* owner);
    ~ModeSyncServer();

    // Start the server thread that listens on the specified TCP port.
    // Returns false if the server is already running or if the thread
    // could not be created.
    bool Start(unsigned short port);

    // Stop the server thread and close any active sockets. This method
    // is safe to call multiple times.
    void Stop();

    // Broadcast the current mode to the connected client (if any).
    // The mode must be in the range [1,3]; out-of-range values are ignored.
    void BroadcastCurrentMode(int mode);

private:
    void ServerThreadProc(unsigned short port);
    void ProcessLine(const std::string& line);

    struct Impl;
    Impl* m_impl;

    TaskTrayApp* m_owner;
    std::thread m_thread;
    std::atomic<bool> m_running;
    std::mutex m_mutex;
};

#endif // MODE_SYNC_SERVER_H
