#include <winsock2.h>
#include <ws2tcpip.h>

#include "ModeSyncServer.h"
#include "TaskTrayApp.h"
#include "DebugLog.h"

#include <string>
#include <sstream>
#include <cstring>
#include <chrono>
#include <thread>

#pragma comment(lib, "Ws2_32.lib")

// Internal implementation structure that holds socket handles and
// buffering state.
struct ModeSyncServer::Impl
{
    SOCKET listenSocket = INVALID_SOCKET;
    SOCKET clientSocket = INVALID_SOCKET;
    std::string recvBuffer;
};

ModeSyncServer::ModeSyncServer(TaskTrayApp* owner)
    : m_impl(new Impl())
    , m_owner(owner)
    , m_running(false)
{
}

ModeSyncServer::~ModeSyncServer()
{
    Stop();

    std::lock_guard<std::mutex> lock(m_mutex);
    delete m_impl;
    m_impl = nullptr;
}

bool ModeSyncServer::Start(unsigned short port)
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        DebugLog("ModeSyncServer::Start: already running.");
        return false;
    }

    try {
        m_thread = std::thread(&ModeSyncServer::ServerThreadProc, this, port);
    }
    catch (const std::exception& e) {
        DebugLog(std::string("ModeSyncServer::Start: failed to create thread: ") + e.what());
        m_running.store(false);
        return false;
    }
    catch (...) {
        DebugLog("ModeSyncServer::Start: failed to create thread (unknown error).");
        m_running.store(false);
        return false;
    }

    return true;
}

void ModeSyncServer::Stop()
{
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false)) {
        // Not running.
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl) {
            if (m_impl->clientSocket != INVALID_SOCKET) {
                shutdown(m_impl->clientSocket, SD_BOTH);
                closesocket(m_impl->clientSocket);
                m_impl->clientSocket = INVALID_SOCKET;
            }
            if (m_impl->listenSocket != INVALID_SOCKET) {
                shutdown(m_impl->listenSocket, SD_BOTH);
                closesocket(m_impl->listenSocket);
                m_impl->listenSocket = INVALID_SOCKET;
            }
        }
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ModeSyncServer::BroadcastCurrentMode(int mode)
{
    if (mode < 1 || mode > 3) {
        DebugLog("ModeSyncServer::BroadcastCurrentMode: invalid mode: " + std::to_string(mode));
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl || m_impl->clientSocket == INVALID_SOCKET) {
        return;
    }

    std::string line = "MODE " + std::to_string(mode) + "\n";

    int totalSent = 0;
    while (totalSent < static_cast<int>(line.size())) {
        int sent = send(m_impl->clientSocket,
                        line.data() + totalSent,
                        static_cast<int>(line.size()) - totalSent,
                        0);
        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            DebugLog("ModeSyncServer::BroadcastCurrentMode: send failed: " + std::to_string(err));
            break;
        }
        if (sent == 0) {
            break;
        }
        totalSent += sent;
    }
}

void ModeSyncServer::ServerThreadProc(unsigned short port)
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        DebugLog("ModeSyncServer::ServerThreadProc: WSAStartup failed: " + std::to_string(result));
        m_running.store(false);
        return;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        int err = WSAGetLastError();
        DebugLog("ModeSyncServer::ServerThreadProc: socket() failed: " + std::to_string(err));
        WSACleanup();
        m_running.store(false);
        return;
    }

    // Allow address reuse to make restart smoother.
    BOOL reuse = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        DebugLog("ModeSyncServer::ServerThreadProc: bind() failed: " + std::to_string(err));
        closesocket(listenSocket);
        WSACleanup();
        m_running.store(false);
        return;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        DebugLog("ModeSyncServer::ServerThreadProc: listen() failed: " + std::to_string(err));
        closesocket(listenSocket);
        WSACleanup();
        m_running.store(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl) {
            m_impl->listenSocket = listenSocket;
            m_impl->clientSocket = INVALID_SOCKET;
            m_impl->recvBuffer.clear();
        }
    }

    DebugLog("ModeSyncServer::ServerThreadProc: listening for connections.");

    while (m_running.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);

        SOCKET maxSock = listenSocket;
        FD_SET(listenSocket, &readSet);

        SOCKET clientSocketCopy = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_impl && m_impl->clientSocket != INVALID_SOCKET) {
                clientSocketCopy = m_impl->clientSocket;
            }
        }

        if (clientSocketCopy != INVALID_SOCKET) {
            FD_SET(clientSocketCopy, &readSet);
            if (clientSocketCopy > maxSock) {
                maxSock = clientSocketCopy;
            }
        }

        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(static_cast<int>(maxSock + 1), &readSet, nullptr, nullptr, &tv);
        if (sel == SOCKET_ERROR) {
            int err = WSAGetLastError();
            DebugLog("ModeSyncServer::ServerThreadProc: select() failed: " + std::to_string(err));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (sel == 0) {
            // timeout
            continue;
        }

        // New incoming connection?
        if (FD_ISSET(listenSocket, &readSet)) {
            sockaddr_in clientAddr;
            int clientAddrLen = sizeof(clientAddr);
            SOCKET newClient = accept(listenSocket,
                                      reinterpret_cast<sockaddr*>(&clientAddr),
                                      &clientAddrLen);
            if (newClient == INVALID_SOCKET) {
                int err = WSAGetLastError();
                DebugLog("ModeSyncServer::ServerThreadProc: accept() failed: " + std::to_string(err));
            } else {
                DebugLog("ModeSyncServer::ServerThreadProc: client connected.");

                SOCKET oldClient = INVALID_SOCKET;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_impl) {
                        oldClient = m_impl->clientSocket;
                        m_impl->clientSocket = newClient;
                        m_impl->recvBuffer.clear();
                    }
                }

                if (oldClient != INVALID_SOCKET) {
                    shutdown(oldClient, SD_BOTH);
                    closesocket(oldClient);
                }

                // Send current mode to the new client.
                if (m_owner) {
                    int mode = m_owner->GetOptimizedPlanForSync();
                    if (mode >= 1 && mode <= 3) {
                        BroadcastCurrentMode(mode);
                    }
                }
            }
        }

        // Data from client?
        if (clientSocketCopy != INVALID_SOCKET && FD_ISSET(clientSocketCopy, &readSet)) {
            char buf[512];
            int received = recv(clientSocketCopy, buf, sizeof(buf), 0);
            if (received == SOCKET_ERROR) {
                int err = WSAGetLastError();
                DebugLog("ModeSyncServer::ServerThreadProc: recv() failed: " + std::to_string(err));

                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_impl && m_impl->clientSocket != INVALID_SOCKET) {
                    shutdown(m_impl->clientSocket, SD_BOTH);
                    closesocket(m_impl->clientSocket);
                    m_impl->clientSocket = INVALID_SOCKET;
                    m_impl->recvBuffer.clear();
                }
            } else if (received == 0) {
                DebugLog("ModeSyncServer::ServerThreadProc: client disconnected.");

                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_impl && m_impl->clientSocket != INVALID_SOCKET) {
                    shutdown(m_impl->clientSocket, SD_BOTH);
                    closesocket(m_impl->clientSocket);
                    m_impl->clientSocket = INVALID_SOCKET;
                    m_impl->recvBuffer.clear();
                }
            } else {
                std::string chunk(buf, buf + received);

                std::string combined;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_impl) {
                        m_impl->recvBuffer.append(chunk);
                        combined.swap(m_impl->recvBuffer);
                    }
                }

                size_t start = 0;
                while (start < combined.size()) {
                    size_t pos = combined.find('\n', start);
                    if (pos == std::string::npos) {
                        // Put back partial line for the next recv.
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_impl) {
                            m_impl->recvBuffer.assign(combined.substr(start));
                        }
                        break;
                    }

                    std::string line = combined.substr(start, pos - start);
                    start = pos + 1;

                    // Trim CR if present.
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }

                    ProcessLine(line);
                }
            }
        }
    }

    DebugLog("ModeSyncServer::ServerThreadProc: shutting down.");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl) {
            if (m_impl->clientSocket != INVALID_SOCKET) {
                shutdown(m_impl->clientSocket, SD_BOTH);
                closesocket(m_impl->clientSocket);
                m_impl->clientSocket = INVALID_SOCKET;
            }
            if (m_impl->listenSocket != INVALID_SOCKET) {
                shutdown(m_impl->listenSocket, SD_BOTH);
                closesocket(m_impl->listenSocket);
                m_impl->listenSocket = INVALID_SOCKET;
            }
        }
    }

    WSACleanup();
    m_running.store(false);
}

void ModeSyncServer::ProcessLine(const std::string& line)
{
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "MODE") {
        int mode = 0;
        iss >> mode;
        if (!iss.fail() && mode >= 1 && mode <= 3) {
            if (m_owner) {
                m_owner->UpdateOptimizedPlanFromNetwork(mode);
            }
        } else {
            DebugLog("ModeSyncServer::ProcessLine: invalid MODE line: " + line);
        }
    } else {
        // Unknown command; ignore for forward compatibility.
    }
}
