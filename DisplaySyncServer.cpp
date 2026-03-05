#include <winsock2.h>
#include <ws2tcpip.h>

#include "DisplaySyncServer.h"
#include "TaskTrayApp.h"
#include "DebugLog.h"
#include "SecureLineCrypto.h"

#include <string>
#include <sstream>
#include <cstring>

#pragma comment(lib, "Ws2_32.lib")

// Internal implementation structure that holds socket handles.
struct DisplaySyncServer::Impl
{
    SOCKET listenSocket = INVALID_SOCKET;
    SOCKET clientSocket = INVALID_SOCKET;
    hk_secureline::Session session;
};

static bool SendAll(SOCKET sock, const char* data, int len)
{
    int sentTotal = 0;
    while (sentTotal < len) {
        int sent = send(sock, data + sentTotal, len - sentTotal, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        sentTotal += sent;
    }
    return true;
}

DisplaySyncServer::DisplaySyncServer(TaskTrayApp* owner)
    : m_impl(new Impl()),
      m_owner(owner),
      m_running(false)
{
}

DisplaySyncServer::~DisplaySyncServer()
{
    Stop();

    std::lock_guard<std::mutex> lock(m_mutex);
    delete m_impl;
    m_impl = nullptr;
}

bool DisplaySyncServer::Start(unsigned short port)
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        DebugLog("DisplaySyncServer::Start: already running.");
        return false;
    }

    try {
        m_thread = std::thread(&DisplaySyncServer::ServerThreadProc, this, port);
    }
    catch (const std::exception& e) {
        DebugLog(std::string("DisplaySyncServer::Start: failed to create thread: ") + e.what());
        m_running.store(false);
        return false;
    }

    return true;
}

void DisplaySyncServer::Stop()
{
    if (!m_running.exchange(false)) {
        // Not running
        return;
    }

    // Close sockets to unblock select()/accept()/recv().
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl) {
            if (m_impl->listenSocket != INVALID_SOCKET) {
                closesocket(m_impl->listenSocket);
                m_impl->listenSocket = INVALID_SOCKET;
            }
            if (m_impl->clientSocket != INVALID_SOCKET) {
                closesocket(m_impl->clientSocket);
                m_impl->clientSocket = INVALID_SOCKET;
            }
            hk_secureline::Clear(m_impl->session);
        }
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void DisplaySyncServer::BroadcastCurrentState()
{
    if (!m_running.load()) {
        return;
    }
    if (!m_owner) {
        return;
    }

    int displayCount = 0;
    int activeIndex = -1;
    m_owner->GetDisplayStateForSync(displayCount, activeIndex);

    std::ostringstream oss;
    oss << "STATE " << displayCount << " " << activeIndex;

    SOCKET clientSocket = INVALID_SOCKET;
    hk_secureline::Session sessionCopy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl) {
            clientSocket = m_impl->clientSocket;
            sessionCopy = m_impl->session;
        }
    }

    if (clientSocket == INVALID_SOCKET || !sessionCopy.active) {
        return;
    }

    std::string secLine;
    if (!hk_secureline::EncryptLineToSec1(sessionCopy, oss.str(), secLine)) {
        DebugLog("DisplaySyncServer::BroadcastCurrentState: EncryptLineToSec1 failed.");
        return;
    }

    if (!SendAll(clientSocket, secLine.c_str(), static_cast<int>(secLine.size()))) {
        int err = WSAGetLastError();
        DebugLog("DisplaySyncServer::BroadcastCurrentState: send failed: " + std::to_string(err));

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl && m_impl->clientSocket == clientSocket) {
            closesocket(m_impl->clientSocket);
            m_impl->clientSocket = INVALID_SOCKET;
            hk_secureline::Clear(m_impl->session);
        }
    }
}

void DisplaySyncServer::ServerThreadProc(unsigned short port)
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        DebugLog("DisplaySyncServer::ServerThreadProc: WSAStartup failed: " + std::to_string(result));
        m_running.store(false);
        return;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        int err = WSAGetLastError();
        DebugLog("DisplaySyncServer::ServerThreadProc: socket() failed: " + std::to_string(err));
        WSACleanup();
        m_running.store(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl) {
            m_impl->listenSocket = listenSocket;
        }
    }

    // Allow quick reuse of the address if the process is restarted.
    BOOL reuse = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
    addr.sin_port = htons(port);

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        DebugLog("DisplaySyncServer::ServerThreadProc: bind() failed: " + std::to_string(err));
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_impl && m_impl->listenSocket == listenSocket) {
                closesocket(m_impl->listenSocket);
                m_impl->listenSocket = INVALID_SOCKET;
            }
        }
        WSACleanup();
        m_running.store(false);
        return;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        DebugLog("DisplaySyncServer::ServerThreadProc: listen() failed: " + std::to_string(err));
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_impl && m_impl->listenSocket == listenSocket) {
                closesocket(m_impl->listenSocket);
                m_impl->listenSocket = INVALID_SOCKET;
            }
        }
        WSACleanup();
        m_running.store(false);
        return;
    }

    DebugLog("DisplaySyncServer: Listening on TCP port " + std::to_string(port) + ".");

    while (m_running.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);

        timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int sel = select(0, &readSet, nullptr, nullptr, &timeout);
        if (sel == SOCKET_ERROR) {
            int err = WSAGetLastError();
            DebugLog("DisplaySyncServer::ServerThreadProc: select() failed: " + std::to_string(err));
            break;
        }

        if (sel == 0) {
            continue;
        }

        if (!FD_ISSET(listenSocket, &readSet)) {
            continue;
        }

        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSocket == INVALID_SOCKET) {
            int err = WSAGetLastError();
            DebugLog("DisplaySyncServer::ServerThreadProc: accept() failed: " + std::to_string(err));
            continue;
        }

        DebugLog("DisplaySyncServer: Client connected (starting secure handshake).");

        // Replace previous client.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_impl) {
                if (m_impl->clientSocket != INVALID_SOCKET) {
                    closesocket(m_impl->clientSocket);
                    m_impl->clientSocket = INVALID_SOCKET;
                }
                hk_secureline::Clear(m_impl->session);
                m_impl->clientSocket = clientSocket;
            }
        }

        // Secure handshake (HELLO1/WELCOME1).
        std::string recvBuffer;
        hk_secureline::Session session;
        if (!hk_secureline::ServerHandshake(clientSocket, recvBuffer, session, 5000)) {
            DebugLog("DisplaySyncServer: secure handshake failed; closing client.");
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_impl && m_impl->clientSocket == clientSocket) {
                    closesocket(m_impl->clientSocket);
                    m_impl->clientSocket = INVALID_SOCKET;
                    hk_secureline::Clear(m_impl->session);
                }
            }
            closesocket(clientSocket);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_impl && m_impl->clientSocket == clientSocket) {
                m_impl->session = session;
            }
        }

        DebugLog("DisplaySyncServer: secure handshake complete.");

        // Send initial state.
        BroadcastCurrentState();

        // Receive loop for this client.
        char buffer[512];

        while (m_running.load()) {
            int received = recv(clientSocket, buffer, sizeof(buffer), 0);
            if (received == 0) {
                DebugLog("DisplaySyncServer: Client disconnected.");
                break;
            }
            if (received == SOCKET_ERROR) {
                int err = WSAGetLastError();
                DebugLog("DisplaySyncServer::ServerThreadProc: recv() failed: " + std::to_string(err));
                break;
            }

            recvBuffer.append(buffer, received);

            for (;;) {
                std::size_t pos = recvBuffer.find('\n');
                if (pos == std::string::npos) {
                    break;
                }

                std::string line = recvBuffer.substr(0, pos);
                recvBuffer.erase(0, pos + 1);

                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.empty()) {
                    continue;
                }

                std::string plain;
                if (!hk_secureline::DecryptSec1ToLine(session, line, plain)) {
                    DebugLog("DisplaySyncServer: invalid SEC1 line; closing client.");
                    goto client_done;
                }

                if (plain == "DISCONNECT") {
                    DebugLog("DisplaySyncServer: received DISCONNECT.");
                    goto client_done;
                }

                std::istringstream iss(plain);
                std::string command;
                iss >> command;

                if (command == "SELECT") {
                    int index = -1;
                    if (iss >> index) {
                        if (index >= 0 && index < 4) {
                            DebugLog("DisplaySyncServer: Received SELECT command. index=" + std::to_string(index));
                            if (m_owner) {
                                m_owner->SelectDisplay(index);
                                BroadcastCurrentState();
                            }
                        }
                        else {
                            DebugLog("DisplaySyncServer: SELECT index out of range: " + std::to_string(index));
                        }
                    }
                    else {
                        DebugLog("DisplaySyncServer: Failed to parse SELECT command: \"" + plain + "\"");
                    }
                }
                else {
                    DebugLog("DisplaySyncServer: Unknown command from client: \"" + plain + "\"");
                }
            }
        }

client_done:
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_impl && m_impl->clientSocket == clientSocket) {
                closesocket(m_impl->clientSocket);
                m_impl->clientSocket = INVALID_SOCKET;
                hk_secureline::Clear(m_impl->session);
            }
        }

        closesocket(clientSocket);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_impl && m_impl->listenSocket != INVALID_SOCKET) {
            closesocket(m_impl->listenSocket);
            m_impl->listenSocket = INVALID_SOCKET;
        }
    }

    WSACleanup();
    m_running.store(false);

    DebugLog("DisplaySyncServer: Server thread exiting.");
}
