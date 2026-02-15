#ifndef DISPLAY_SYNC_SERVER_H
#define DISPLAY_SYNC_SERVER_H

#include <thread>
#include <atomic>
#include <mutex>

class TaskTrayApp;

// TCP server running in the task tray process to synchronize the
// "Select Display" state with remote clients (Qt client application).
//
// Protocol (line based, UTF-8, '\n' terminated):
//   Client -> Server:
//     SELECT <index>        # 0-based display index to select
//
//   Server -> Client:
//     STATE <count> <index> # <count> = number of displays (0-4),
//                           # <index> = currently selected display index (0-based, -1 if none)
//
class DisplaySyncServer
{
public:
    explicit DisplaySyncServer(TaskTrayApp* owner);
    ~DisplaySyncServer();

    // Start listening on the given TCP port (e.g. 8500).
    // Returns true on success, false if the server is already running or initialization fails.
    bool Start(unsigned short port);

    // Stop the server thread and close all sockets.
    // It is safe to call this multiple times.
    void Stop();

    // Called by TaskTrayApp when the display configuration or selected display changes.
    // This sends the latest STATE line to the connected client, if any.
    void BroadcastCurrentState();

private:
    void ServerThreadProc(unsigned short port);

    struct Impl;
    Impl* m_impl;

    TaskTrayApp* m_owner;
    std::thread m_thread;
    std::atomic<bool> m_running;
    std::mutex m_mutex;
};

#endif // DISPLAY_SYNC_SERVER_H
