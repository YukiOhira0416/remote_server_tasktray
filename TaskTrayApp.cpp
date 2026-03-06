#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "TaskTrayApp.h"
#include "StringConversion.h"
#include "Utility.h"
#include "SharedMemoryHelper.h"
#include <tchar.h>
#include <iostream>
#include <algorithm>
#include <functional>
#include <windows.h>
#include <vector>
#include <string>
#include <CommCtrl.h>
#include <winsvc.h>
#include "DebugLog.h"
#include "Globals.h"
#include "OverlayManager.h"
#include "DisplaySyncServer.h"
#include "ModeSyncServer.h"
#include "DeviceKeyCrypto.h"
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <dbt.h>
#include <atomic>
#include <thread>
#include <memory>
#include <cstdint>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QWidget>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QPushButton>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/Qt>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include "ui_Main_UI.h"

#include <wincrypt.h>
#include <winhttp.h>
#include <iphlpapi.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")


namespace {
constexpr UINT ID_EXIT = 1;
constexpr UINT ID_DISPLAY_STATUS = 50;
constexpr UINT ID_DISPLAY_BASE = 100;
constexpr UINT ID_CAPTURE_MODE_NORMAL = 200;
constexpr UINT ID_CAPTURE_MODE_GAME = 201;
constexpr UINT ID_CONTROL_PANEL = 300;
constexpr int MAX_DISPLAY_MENU_ITEMS = 4; // Max items in "Select Display" submenu

std::atomic<bool> g_controlPanelRunning{ false };
std::atomic<QMainWindow*> g_controlPanelWindow{ nullptr };
std::atomic<std::uint64_t> g_controlPanelToken{ 0 };
std::atomic<TaskTrayApp*> g_taskTrayAppInstance{ nullptr };

class ControlPanelCloseFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event && event->type() == QEvent::Close) {
            if (auto* window = qobject_cast<QWidget*>(watched)) {
                DebugLog("ControlPanelCloseFilter: Close event intercepted. Hiding control panel instead of closing.");
                event->ignore();
                window->hide();
                window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
            }
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
};

struct ServerActivationConfig {
    std::string serverName;
    std::string userEmail;        // UserID (email)
    std::string password;
    std::string activationCode;
    std::string machineId;
    std::string lanIp;
    bool activated = false;
};

static std::filesystem::path GetRoamingDir(const std::wstring& subDir) {
    wchar_t* appdata = nullptr;
    size_t len = 0;
    _wdupenv_s(&appdata, &len, L"APPDATA");
    std::filesystem::path base = appdata ? std::filesystem::path(appdata) : std::filesystem::path();
    if (appdata) free(appdata);
    return base / L"HayateKomorebi" / subDir;
}

static std::filesystem::path GetServerConfigPath() {
    return GetRoamingDir(L"Server") / L"Server"; // File A: no extension
}

static std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

static std::string ExecCmdCapture(const std::string& cmd) {
    std::string out;
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return out;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        out.append(buf);
    }
    _pclose(pipe);
    return out;
}

static std::string GetMachineId() {
    // Best-effort: combine CPU ProcessorId and csproduct UUID.
    // If either fails, fall back to UUID only.
    std::string cpu = ExecCmdCapture("wmic cpu get ProcessorId /value 2>nul");
    std::string uuid = ExecCmdCapture("wmic csproduct get UUID /value 2>nul");

    auto extractVal = [](const std::string& text, const std::string& key) -> std::string {
        auto pos = text.find(key + "=");
        if (pos == std::string::npos) return "";
        pos += key.size() + 1;
        auto end = text.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = text.size();
        return Trim(text.substr(pos, end - pos));
    };

    std::string cpuId = extractVal(cpu, "ProcessorId");
    std::string sysUuid = extractVal(uuid, "UUID");
    if (sysUuid.empty()) {
        // Some WMIC outputs are table-like; try a simpler parse.
        std::istringstream iss(uuid);
        std::string line;
        while (std::getline(iss, line)) {
            line = Trim(line);
            if (line.empty()) continue;
            if (line.find("UUID") != std::string::npos) continue;
            sysUuid = line;
            break;
        }
    }
    if (cpuId.empty()) cpuId = "UNKNOWNCPU";
    if (sysUuid.empty()) sysUuid = "UNKNOWNUUID";
    return cpuId + "-" + sysUuid;
}

static std::string GetLanIpv4() {
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &bufLen);
    if (bufLen == 0) return "";
    std::vector<unsigned char> buf(bufLen);
    IP_ADAPTER_ADDRESSES* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &bufLen);
    if (ret != NO_ERROR) return "";
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr) continue;
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            SOCKADDR_IN* sin = reinterpret_cast<SOCKADDR_IN*>(ua->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            std::string s(ip);
            if (s.rfind("169.254.", 0) == 0) continue; // skip APIPA
            if (s == "127.0.0.1") continue;
            return s;
        }
    }
    return "";
}

static bool DpapiEncrypt(const std::string& plain, const std::string& entropy, std::string& outCipher) {
    DATA_BLOB inBlob{0};
    inBlob.pbData = (BYTE*)plain.data();
    inBlob.cbData = (DWORD)plain.size();

    DATA_BLOB entBlob{0};
    entBlob.pbData = (BYTE*)entropy.data();
    entBlob.cbData = (DWORD)entropy.size();

    DATA_BLOB outBlob{0};
    if (!CryptProtectData(&inBlob, L"HayateKomorebi", &entBlob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return false;
    }
    outCipher.assign((char*)outBlob.pbData, (size_t)outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

static bool DpapiDecrypt(const std::string& cipher, const std::string& entropy, std::string& outPlain) {
    DATA_BLOB inBlob{0};
    inBlob.pbData = (BYTE*)cipher.data();
    inBlob.cbData = (DWORD)cipher.size();

    DATA_BLOB entBlob{0};
    entBlob.pbData = (BYTE*)entropy.data();
    entBlob.cbData = (DWORD)entropy.size();

    DATA_BLOB outBlob{0};
    LPWSTR desc = nullptr;
    if (!CryptUnprotectData(&inBlob, &desc, &entBlob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        if (desc) LocalFree(desc);
        return false;
    }
    if (desc) LocalFree(desc);
    outPlain.assign((char*)outBlob.pbData, (size_t)outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

static std::string SerializeConfig(const ServerActivationConfig& c) {
    std::ostringstream oss;

    // Always persist ServerName (required before activation is allowed).
    oss << "ServerName=" << c.serverName << "\n";

    // File minimization:
    // If NOT activated and all secrets are cleared, keep only ServerName in the file.
    const bool secretsEmpty =
        c.userEmail.empty() &&
        c.password.empty() &&
        c.activationCode.empty() &&
        c.machineId.empty() &&
        c.lanIp.empty();

    if (!c.activated && secretsEmpty) {
        return oss.str();
    }

    if (!c.userEmail.empty())      oss << "UserEmail=" << c.userEmail << "\n";
    if (!c.password.empty())       oss << "Password=" << c.password << "\n";
    if (!c.activationCode.empty()) oss << "ActivationCode=" << c.activationCode << "\n";
    if (!c.machineId.empty())      oss << "MachineId=" << c.machineId << "\n";
    if (!c.lanIp.empty())          oss << "LanIp=" << c.lanIp << "\n";

    // Keep an explicit Activated field when secrets exist (or activated).
    oss << "Activated=" << (c.activated ? "1" : "0") << "\n";
    return oss.str();
}

static ServerActivationConfig ParseConfig(const std::string& text) {
    ServerActivationConfig c;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "ServerName") c.serverName = v;
        else if (k == "UserEmail") c.userEmail = v;
        else if (k == "Password") c.password = v;
        else if (k == "ActivationCode") c.activationCode = v;
        else if (k == "MachineId") c.machineId = v;
        else if (k == "LanIp") c.lanIp = v;
        else if (k == "Activated") c.activated = (v == "1");
    }
    return c;
}

static bool LoadServerConfig(ServerActivationConfig& out) {
    auto path = GetServerConfigPath();
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::string cipher((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (cipher.empty()) return false;

    // Decrypt using hardware-derived MachineId (do not depend on file contents).
    std::string machineId = GetMachineId();
    std::string plain;
    if (!DpapiDecrypt(cipher, machineId, plain)) {
        return false;
    }

    out = ParseConfig(plain);

    // Ensure derived fields exist even when config file is minimized.
    if (out.machineId.empty()) out.machineId = machineId;
    if (out.lanIp.empty()) out.lanIp = GetLanIpv4();
    return true;
}

static bool SaveServerConfig(const ServerActivationConfig& cfg) {
    auto dir = GetRoamingDir(L"Server");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    (void)ec;
    auto path = GetServerConfigPath();

    std::string plain = SerializeConfig(cfg);
    std::string cipher;

    // Encryption entropy is derived from hardware MachineId (CPU + motherboard UUID).
    // This allows the config file to omit MachineId when minimized.
    const std::string entropy = GetMachineId();
    if (!DpapiEncrypt(plain, entropy, cipher)) {
        return false;
    }

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(cipher.data(), (std::streamsize)cipher.size());
    return ofs.good();
}

static std::wstring GetEnvW(const wchar_t* name, const std::wstring& def) {
    wchar_t* val = nullptr;
    size_t len = 0;
    _wdupenv_s(&val, &len, name);
    std::wstring out = (val && len) ? std::wstring(val) : def;
    if (val) free(val);
    return out;
}

static std::wstring ToW(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), wlen);
    return ws;
}

static std::string FromW(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring GetServiceNameW() {
    // Installer can override via env var.
    return GetEnvW(L"HAYATEKOMOREBI_SERVICE_NAME", L"HayateKomorebiRemoteService");
}

static bool OpenServiceHandles(const std::wstring& serviceName, DWORD desiredAccess, SC_HANDLE& outScm, SC_HANDLE& outSvc) {
    outScm = nullptr;
    outSvc = nullptr;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        DebugLog("ServiceControl: OpenSCManager failed.");
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), desiredAccess);
    if (!svc) {
        DebugLog("ServiceControl: OpenService failed.");
        CloseServiceHandle(scm);
        return false;
    }
    outScm = scm;
    outSvc = svc;
    return true;
}

static bool SetServiceStartType(const std::wstring& serviceName, DWORD startType) {
    SC_HANDLE scm = nullptr;
    SC_HANDLE svc = nullptr;
    if (!OpenServiceHandles(serviceName, SERVICE_CHANGE_CONFIG, scm, svc)) {
        return false;
    }

    const BOOL ok = ChangeServiceConfigW(
        svc,
        SERVICE_NO_CHANGE,
        startType,
        SERVICE_NO_CHANGE,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (!ok) {
        DebugLog("ServiceControl: ChangeServiceConfig failed.");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok == TRUE;
}

static bool StartServiceIfNeeded(const std::wstring& serviceName) {
    SC_HANDLE scm = nullptr;
    SC_HANDLE svc = nullptr;
    if (!OpenServiceHandles(serviceName, SERVICE_QUERY_STATUS | SERVICE_START, scm, svc)) {
        return false;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytesNeeded = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
        DebugLog("ServiceControl: QueryServiceStatusEx failed.");
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    if (ssp.dwCurrentState == SERVICE_RUNNING) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return true;
    }

    if (!StartServiceW(svc, 0, nullptr)) {
        const DWORD err = GetLastError();
        // If it's already starting, treat as success.
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            DebugLog("ServiceControl: StartService failed.");
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static bool StopServiceIfRunning(const std::wstring& serviceName) {
    SC_HANDLE scm = nullptr;
    SC_HANDLE svc = nullptr;
    if (!OpenServiceHandles(serviceName, SERVICE_QUERY_STATUS | SERVICE_STOP, scm, svc)) {
        return false;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytesNeeded = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
        DebugLog("ServiceControl: QueryServiceStatusEx failed (stop).");
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    if (ssp.dwCurrentState == SERVICE_STOPPED) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return true;
    }

    SERVICE_STATUS st{};
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        const DWORD err = GetLastError();
        // If already stopping, treat as success.
        if (err != ERROR_SERVICE_NOT_ACTIVE) {
            DebugLog("ServiceControl: ControlService(STOP) failed.");
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static bool WinHttpPostJson(const std::wstring& baseUrl, const std::wstring& path, const std::string& jsonBody, std::string& outResponse) {
    outResponse.clear();

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    std::wstring url = baseUrl;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        return false;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    INTERNET_PORT port = uc.nPort;
    bool isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"HayateKomorebi/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (isHttps) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    const wchar_t* hdrs = L"Content-Type: application/json\r\n";
    BOOL b = WinHttpSendRequest(hRequest, hdrs, (DWORD)-1L, (LPVOID)jsonBody.data(), (DWORD)jsonBody.size(), (DWORD)jsonBody.size(), 0);
    if (!b) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    b = WinHttpReceiveResponse(hRequest, nullptr);
    if (!b) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    DWORD avail = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
        if (!avail) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
        chunk.resize(read);
        outResponse += chunk;
    } while (avail > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return !outResponse.empty();
}

static std::string JsonEscape(const std::string& s) {
    std::ostringstream oss;
    for (char ch : s) {
        switch (ch) {
            case '\\': oss << "\\\\"; break;
            case '"': oss << "\\\""; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if ((unsigned char)ch < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)(unsigned char)ch;
                } else {
                    oss << ch;
                }
        }
    }
    return oss.str();
}

static bool JsonExtractString(const std::string& json, const std::string& key, std::string& out) {
    out.clear();
    std::string pat = "\"" + key + "\"";
    auto p = json.find(pat);
    if (p == std::string::npos) return false;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == '\t')) p++;
    if (p >= json.size() || json[p] != '"') return false;
    p++;
    std::ostringstream oss;
    while (p < json.size()) {
        char c = json[p++];
        if (c == '"') {
            out = oss.str();
            return true;
        }
        if (c == '\\' && p < json.size()) {
            char e = json[p++];
            switch (e) {
                case '"': oss << '"'; break;
                case '\\': oss << '\\'; break;
                case 'n': oss << '\n'; break;
                case 'r': oss << '\r'; break;
                case 't': oss << '\t'; break;
                default: oss << e; break;
            }
        } else {
            oss << c;
        }
    }
    return false;
}

static void ClearActivationSecrets(ServerActivationConfig& cfg) {
    cfg.userEmail.clear();
    cfg.password.clear();
    cfg.activationCode.clear();
    cfg.lanIp.clear();
    cfg.machineId.clear();
    cfg.activated = false;
}
}


// Register the TaskbarCreated message. This is sent when the taskbar is created (e.g., after an explorer.exe crash).
// We need to handle this to re-add our icon.
UINT WM_TASKBARCREATED = RegisterWindowMessage(_T("TaskbarCreated"));

std::filesystem::path GetExecutablePath() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

TaskTrayApp::TaskTrayApp(HINSTANCE hInst)
    : hInstance(hInst)
    , hwnd(NULL)
    , displaySyncServer(nullptr)
    , modeSyncServer(nullptr)
    , optimizedPlan(1)
    , running(true)
    , cleaned(false)
{
    ZeroMemory(&nid, sizeof(nid));
    g_taskTrayAppInstance.store(this);
}


bool TaskTrayApp::Initialize() {
    // 実行ファイルのパスを取征E
    std::filesystem::path exePath = GetExecutablePath();
    std::filesystem::path logFilePath = exePath / "debuglog_tasktray.log";

    // 既存�EログファイルをバチE��アチE�Eする
    if (std::filesystem::exists(logFilePath)) {
        // 現在の日時を取征E
        std::time_t t = std::time(nullptr);
        std::tm tm;
        localtime_s(&tm, &t);

        // 日付文字�Eを作�E
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d%H%M%S");
        std::string timestamp = oss.str();

        // バックアチE�Eファイル名を作�E
        std::string backupFileName = timestamp + "_debuglog_tasktray.log.back";
        std::filesystem::path backupFilePath = exePath / backupFileName;

        // ファイルをリネ�Eム
        std::filesystem::rename(logFilePath, backupFilePath);

        // バックアチE�Eファイルの数を確認し、Eつを趁E��る場合�E古ぁE��のから削除
        std::vector<std::filesystem::path> backupFiles;
        for (const auto& entry : std::filesystem::directory_iterator(exePath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                const std::string suffix = "_debuglog_tasktray.log.back";
            if (filename.length() >= suffix.length() &&
                filename.compare(filename.length() - suffix.length(), suffix.length(), suffix) == 0) {
                    backupFiles.push_back(entry.path());
                }
            }
        }

        // 日付頁E��ソート（新しい頁E��E
        std::sort(backupFiles.begin(), backupFiles.end(), std::greater<std::filesystem::path>());

        // 5つより多い場合、古ぁE��ァイルを削除
        while (backupFiles.size() > 5) {
            std::filesystem::remove(backupFiles.back());
            backupFiles.pop_back();
        }
    }

    // ここから既存�EコーチE
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = _T("TaskTrayClass");

    if (!RegisterClass(&wc)) {
        return false;
    }

    hwnd = CreateWindow(_T("TaskTrayClass"), _T("Task Tray App"), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, hInstance, this);

    if (!hwnd) return false;

    CreateTrayIcon();

    // 初回チE��スプレイ惁E��取征E
    if (!RefreshDisplayList()) {
        DebugLog("Initialize: RefreshDisplayList failed (Service not ready?). Continue anyway.");
        // We do not abort here, as the service might start later.
    }

    if (!displaySyncServer) {
        displaySyncServer = new DisplaySyncServer(this);
        if (!displaySyncServer->Start(8000)) {
            DebugLog("TaskTrayApp::Initialize: Failed to start DisplaySyncServer on port 8000.");
        }
    }

    if (!modeSyncServer) {
        modeSyncServer = new ModeSyncServer(this);
        if (!modeSyncServer->Start(8100)) {
            DebugLog("TaskTrayApp::Initialize: Failed to start ModeSyncServer on port 8100.");
        }
    }

    // Start activation polling in background (must run even when the Qt control panel is closed).
    StartActivationPollThread();

    return true;
}


void TaskTrayApp::CreateTrayIcon() {
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 1;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpy(nid.szTip, _T("GPU & Display Manager"));

    Shell_NotifyIcon(NIM_ADD, &nid);
}

void TaskTrayApp::StartActivationPollThread() {
    if (activationPollRunning.exchange(true)) {
        return;
    }
    activationPollThread = std::thread(&TaskTrayApp::ActivationPollThreadProc, this);
}

void TaskTrayApp::StopActivationPollThread() {
    if (!activationPollRunning.exchange(false)) {
        return;
    }
    activationPollCv.notify_all();
    if (activationPollThread.joinable()) {
        activationPollThread.join();
    }
}

void TaskTrayApp::ActivationPollThreadProc() {
    // Poll every 60 seconds.
    // - If device missing / mismatched / expired / public key mismatch => clear secrets, require re-activate.
    // - If valid => keep activated; update LAN IP.
    while (activationPollRunning.load()) {
        ServerActivationConfig cfg;
        if (LoadServerConfig(cfg)) {
            // If not configured yet (ServerName missing) => nothing to do.
            if (!cfg.serverName.empty() &&
                !cfg.userEmail.empty() &&
                !cfg.password.empty() &&
                !cfg.activationCode.empty() &&
                !cfg.machineId.empty()) {

                const std::wstring baseUrl = GetEnvW(L"HAYATEKOMOREBI_APP_URL", L"https://myapp.local");
                std::string pollKeyPem;
                (void)DeviceKeyCrypto::GetServerPublicKeyForDb(pollKeyPem);
                std::string body = std::string("{")
                    + "\"email\":\"" + JsonEscape(cfg.userEmail) + "\"," 
                    + "\"password\":\"" + JsonEscape(cfg.password) + "\"," 
                    + "\"activation_code\":\"" + JsonEscape(cfg.activationCode) + "\"," 
                    + "\"role\":\"server\"," 
                    + "\"machine_id\":\"" + JsonEscape(cfg.machineId) + "\"," 
                    + "\"public_key_pem\":\"" + JsonEscape(pollKeyPem) + "\""
                    + "}";

                std::string resp;
                if (WinHttpPostJson(baseUrl, L"/api/device_list.php", body, resp)) {
                    const bool valid = (resp.find("\"valid\":true") != std::string::npos);

                    if (!valid) {
                        if (cfg.activated || !cfg.userEmail.empty() || !cfg.password.empty() || !cfg.activationCode.empty()) {
                            DebugLog("ActivationPoll(Server BG): invalid -> clear secrets and revert service.");
                        }
                        cfg.activated = false;
                        ClearActivationSecrets(cfg);
                        SaveServerConfig(cfg);

                        // Revert service to install state: demand start + stopped.
                        const std::wstring svcName = GetServiceNameW();
                        (void)StopServiceIfRunning(svcName);
                        (void)SetServiceStartType(svcName, SERVICE_DEMAND_START);
                    } else {
                        // Became / remains valid.
                        const bool wasActivated = cfg.activated;
                        cfg.activated = true;

                        // Ensure service is always running after activation.
                        const std::wstring svcName = GetServiceNameW();
                        (void)SetServiceStartType(svcName, SERVICE_AUTO_START);
                        (void)StartServiceIfNeeded(svcName);

                        // Refresh LAN IP and heartbeat the server record on EVERY valid poll.
                        // This keeps devices.updated_at fresh so clients can list only activated/live servers.
                        const std::string currentIp = GetLanIpv4();
                        if (!currentIp.empty()) {
                            cfg.lanIp = currentIp;
                        }
                        SaveServerConfig(cfg);

                        std::string upBody = std::string("{")
                            + "\"email\":\"" + JsonEscape(cfg.userEmail) + "\"," 
                            + "\"password\":\"" + JsonEscape(cfg.password) + "\"," 
                            + "\"activation_code\":\"" + JsonEscape(cfg.activationCode) + "\"," 
                            + "\"role\":\"server\"," 
                            + "\"machine_id\":\"" + JsonEscape(cfg.machineId) + "\"," 
                            + "\"device_name\":\"" + JsonEscape(cfg.serverName) + "\"," 
                            + "\"lan_ip\":\"" + JsonEscape(cfg.lanIp) + "\""
                            + "}";
                        std::string upResp;
                        if (!WinHttpPostJson(baseUrl, L"/api/device_update_name.php", upBody, upResp)) {
                            DebugLog("ActivationPoll(Server BG): device_update_name heartbeat POST failed.");
                        }

                        if (!wasActivated) {
                            DebugLog("ActivationPoll(Server BG): valid -> Activated.");
                        }
                    }
                } else {
                    DebugLog("ActivationPoll(Server BG): device_list POST failed.");
                }
            }
        }

        // Wait for next tick or stop.
        std::unique_lock<std::mutex> lk(activationPollMutex);
        activationPollCv.wait_for(lk, std::chrono::seconds(60), [this]() {
            return !activationPollRunning.load();
        });
    }
}

bool TaskTrayApp::Cleanup() {
    // 2回呼ばれても安�Eにする�E�Exitメニュー + WinMain後�E琁E��ど�E�E
    if (cleaned.exchange(true)) {
        return true;
    }

    // Stop background activation polling first.
    StopActivationPollThread();

    // Clean up overlay windows
    OverlayManager::Instance().Cleanup();

    // タスクトレイアイコンを削除
    Shell_NotifyIcon(NIM_DELETE, &nid);

    // スレチE��の停止を指示
    running = false;

    if (displaySyncServer) {
        displaySyncServer->Stop();
        delete displaySyncServer;
        displaySyncServer = nullptr;
    }

    if (modeSyncServer) {
        modeSyncServer->Stop();
        delete modeSyncServer;
        modeSyncServer = nullptr;
    }

    g_taskTrayAppInstance.store(nullptr);

    // 共有メモリとイベントを削除
    // SharedMemoryHelper is now Open-only and client-side doesn't delete anything.
    // SharedMemoryHelper sharedMemoryHelper;
    // sharedMemoryHelper.DeleteSharedMemory(); // REMOVED
    // sharedMemoryHelper.DeleteEvent(); // REMOVED

    return true;
}

void TaskTrayApp::ShowContextMenu() {
    if (hwnd == nullptr) {
        DebugLog("ShowContextMenu: Error - hwnd is nullptr.");
        return;
    }

    POINT pt;
    if (!GetCursorPos(&pt)) {
        DebugLog("ShowContextMenu: Error - Failed to get cursor position.");
        return;
    }

    HMENU hMenu = CreatePopupMenu();
    if (hMenu == NULL) {
        DebugLog("ShowContextMenu: Error - Failed to create popup menu.");
        return;
    }

    // Add "Display Selection" submenu
    HMENU hSubMenu = CreatePopupMenu();
    if (hSubMenu == NULL) {
        DebugLog("ShowContextMenu: Error - Failed to create submenu.");
        DestroyMenu(hMenu);
        return;
    }

    UpdateDisplayMenu(hSubMenu); // Build the menu from shared memory
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, _T("Select Display"));

    HMENU hCaptureMenu = CreatePopupMenu();
    if (hCaptureMenu == NULL) {
        DebugLog("ShowContextMenu: Error - Failed to create capture submenu.");
        DestroyMenu(hSubMenu);
        DestroyMenu(hMenu);
        return;
    }

    UpdateCaptureModeMenu(hCaptureMenu);
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hCaptureMenu, _T("CaptureMode"));

    AppendMenu(hMenu, MF_STRING, ID_CONTROL_PANEL, _T("ControlPanel"));

    // Add separator and Exit item
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_EXIT, _T("Exit")); // Command ID 1 for Exit

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

void TaskTrayApp::UpdateDisplayMenu(HMENU hMenu) {
    DebugLog("UpdateDisplayMenu: Start updating display menu from shared memory.");

    // Clear any existing menu items.
    while (GetMenuItemCount(hMenu) > 0) {
        RemoveMenu(hMenu, 0, MF_BYPOSITION);
    }

    SharedMemoryHelper sharedMemoryHelper; // No args
    std::string numDisplaysStr = sharedMemoryHelper.ReadSharedMemory("DISP_INFO_NUM");

    if (numDisplaysStr.empty()) {
        DebugLog("UpdateDisplayMenu: Shared Memory not ready (DISP_INFO_NUM empty).");
        AppendMenu(hMenu, MF_STRING | MF_GRAYED, ID_DISPLAY_STATUS, _T("Service not ready (DISP_INFO_NUM empty)"));
        return;
    }

    int numDisplays = 0;
    try {
        numDisplays = std::stoi(numDisplaysStr);
    }
    catch (const std::exception& e) {
        DebugLog("UpdateDisplayMenu: Failed to parse DISP_INFO_NUM: " + std::string(e.what()));
        AppendMenu(hMenu, MF_STRING | MF_GRAYED, 0, _T("Error reading displays"));
        return;
    }

    if (numDisplays > MAX_DISPLAY_MENU_ITEMS) {
        DebugLog("UpdateDisplayMenu: numDisplays (" + std::to_string(numDisplays) +
                 ") exceeds MAX_DISPLAY_MENU_ITEMS (" + std::to_string(MAX_DISPLAY_MENU_ITEMS) + "). Clamping.");
        numDisplays = MAX_DISPLAY_MENU_ITEMS;
    }

    if (numDisplays == 0) {
        AppendMenu(hMenu, MF_STRING | MF_GRAYED, ID_DISPLAY_STATUS, _T("No displays found (DISP_INFO_NUM=0)"));
        AppendMenu(hMenu, MF_STRING | MF_GRAYED, ID_DISPLAY_STATUS + 1, _T("If server is running, check shared-memory permission (service security descriptor / integrity level)."));
        return;
    }

    // Get currently selected monitor DeviceID
    std::string selectedDisplaySerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    DebugLog("UpdateDisplayMenu: Currently selected display serial: " + selectedDisplaySerial);

    for (int idx = 0; idx < numDisplays; ++idx) {
        // Read DeviceID (e.g., MONITOR\GSM5B09\...)
        std::string keyID = "DISP_INFO_" + std::to_string(idx);
        std::string currentDisplaySerial = sharedMemoryHelper.ReadSharedMemory(keyID);

        // Menu label: use a stable "Display N" so the list text matches the hover overlay number.
        // (The overlay number is also 1-based: idx + 1)
        std::wstring displayNameW = L"Display " + std::to_wstring(idx + 1);

        UINT flags = MF_STRING;
        // Check if this is the selected one
        if (!currentDisplaySerial.empty() && currentDisplaySerial == selectedDisplaySerial) {
            flags |= MF_CHECKED;
        }

        UINT commandId = ID_DISPLAY_BASE + idx; // Menu command IDs are 0-indexed.

        if (!AppendMenu(hMenu, flags, commandId, displayNameW.c_str())) {
            DebugLog("UpdateDisplayMenu: Failed to add menu item for Display " + std::to_string(idx + 1));
        }
    }

    DebugLog("UpdateDisplayMenu: Finished updating display menu.");
    if (displaySyncServer) {
        displaySyncServer->BroadcastCurrentState();
    }
}

void TaskTrayApp::GetDisplayStateForSync(int& outDisplayCount, int& outActiveDisplayIndex)
{
    outDisplayCount = 0;
    outActiveDisplayIndex = -1;

    SharedMemoryHelper sharedMemoryHelper; // No args

    std::string numDisplaysStr = sharedMemoryHelper.ReadSharedMemory("DISP_INFO_NUM");
    int numDisplays = 0;
    if (!numDisplaysStr.empty()) {
        try {
            numDisplays = std::stoi(numDisplaysStr);
        }
        catch (...) {
            numDisplays = 0;
        }
    }

    if (numDisplays < 0) {
        numDisplays = 0;
    }
    if (numDisplays > MAX_DISPLAY_MENU_ITEMS) {
        numDisplays = MAX_DISPLAY_MENU_ITEMS;
    }

    outDisplayCount = numDisplays;

    if (numDisplays <= 0) {
        return;
    }

    std::string selectedSerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    if (selectedSerial.empty()) {
        return;
    }

    for (int idx = 0; idx < numDisplays; ++idx) {
        std::string key = "DISP_INFO_" + std::to_string(idx);
        std::string serial = sharedMemoryHelper.ReadSharedMemory(key);
        if (!serial.empty() && serial == selectedSerial) {
            outActiveDisplayIndex = idx; // 0-based index
            break;
        }
    }
}

void TaskTrayApp::SelectDisplay(int displayIndex) {
    // displayIndex is 0-based from the menu and maps to DISP_INFO_{index}
    DebugLog("SelectDisplay: User selected display at index " + std::to_string(displayIndex));

    SharedMemoryHelper sharedMemoryHelper; // No args

    // Read the serial number for the selected index from shared memory (0-based)
    std::string key = "DISP_INFO_" + std::to_string(displayIndex);
    std::string selectedSerial = sharedMemoryHelper.ReadSharedMemory(key);

    if (selectedSerial.empty()) {
        DebugLog("SelectDisplay: Could not find serial number for display index " + std::to_string(displayIndex) + " with key " + key);
        return;
    }

    // Persist the new selection to shared memory
    if (sharedMemoryHelper.WriteSharedMemory("DISP_INFO", selectedSerial)) {
        DebugLog("SelectDisplay: New display selected. Serial: " + selectedSerial);

        // Signal the event to notify the service
        sharedMemoryHelper.SignalEvent("DISP_INFO");

        // Update the tray icon tooltip to reflect the new selection
        std::wstring newTooltip = L"Display Manager - Selected: Display " + std::to_wstring(displayIndex + 1);
        UpdateTrayTooltip(newTooltip);
        if (displaySyncServer) {
            displaySyncServer->BroadcastCurrentState();
        }
    } else {
        DebugLog("SelectDisplay: Failed to write to shared memory (Service not ready?).");
    }
}

void TaskTrayApp::SetCaptureMode(int mode) {
    SharedMemoryHelper sharedMemoryHelper; // No args
    std::string modeValue = std::to_string(mode);
    DebugLog("SetCaptureMode: Setting capture mode to " + modeValue);
    if (sharedMemoryHelper.WriteSharedMemory("Capture_Mode", modeValue)) {
        // REBOOT とレジストリ反映はサービス側に雁E��E��めE
    } else {
        DebugLog("SetCaptureMode: Failed to write to shared memory (Service not ready?).");
    }
}

void TaskTrayApp::UpdateOptimizedPlanFromUi(int plan) {
    if (plan < 1 || plan > 3) {
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromUi: invalid plan: " + std::to_string(plan));
        return;
    }

    optimizedPlan.store(plan);
    DebugLog("TaskTrayApp::UpdateOptimizedPlanFromUi: updating plan to " + std::to_string(plan));

    SharedMemoryHelper sharedMemoryHelper;
    std::string value = std::to_string(plan);
    DebugLog("TaskTrayApp::UpdateOptimizedPlanFromUi: Setting OptimizedPlan to " + value);
    if (!sharedMemoryHelper.WriteSharedMemory("OptimizedPlan", value)) {
        DWORD err = GetLastError();
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromUi: Failed to write OptimizedPlan (service not ready?). err=" + std::to_string(err));
    }
    else {
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromUi: OptimizedPlan written to shared memory.");
    }

    // サーバ�E側と他�Eクライアントへ現在のモードを通知
    if (modeSyncServer) {
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromUi: broadcasting mode to clients.");
        modeSyncServer->BroadcastCurrentMode(plan);
    }
    else {
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromUi: modeSyncServer is null; cannot broadcast.");
    }

    // 既に UI 上�EチェチE��ボックスは Save を押した時点で反映済みだが、E
    // 他経路から呼ばれた場合にも確実に UI が現在の plan を示すよぁE��しておく、E
    ApplyOptimizedPlanToUi(plan);
}


void TaskTrayApp::UpdateOptimizedPlanFromNetwork(int plan) {
    if (plan < 1 || plan > 3) {
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromNetwork: invalid plan: " + std::to_string(plan));
        return;
    }

    optimizedPlan.store(plan);
    DebugLog("TaskTrayApp::UpdateOptimizedPlanFromNetwork: updating plan to " + std::to_string(plan));

    SharedMemoryHelper sharedMemoryHelper;
    std::string value = std::to_string(plan);
    DebugLog("TaskTrayApp::UpdateOptimizedPlanFromNetwork: Setting OptimizedPlan to " + value + " (from network)");
    if (!sharedMemoryHelper.WriteSharedMemory("OptimizedPlan", value)) {
        DWORD err = GetLastError();
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromNetwork: Failed to write OptimizedPlan (service not ready?). err=" + std::to_string(err));
    }
    else {
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromNetwork: OptimizedPlan written to shared memory (from network).");
    }

    // ネットワークから受信した plan をタスクトレイの Qt コントロールパネルに反映
    ApplyOptimizedPlanToUi(plan);

    if (modeSyncServer) {
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromNetwork: broadcasting mode to other clients.");
        modeSyncServer->BroadcastCurrentMode(plan);
    }
    else {
        DebugLog("TaskTrayApp::UpdateOptimizedPlanFromNetwork: modeSyncServer is null; cannot broadcast.");
    }
}


int TaskTrayApp::GetOptimizedPlanForSync() const {
    int plan = optimizedPlan.load();
    if (plan < 1 || plan > 3) {
        plan = 1;
    }
    return plan;
}

void TaskTrayApp::ApplyOptimizedPlanToUi(int plan) {
    if (plan < 1 || plan > 3) {
        return;
    }

    QMainWindow* window = g_controlPanelWindow.load();
    if (!window) {
        return;
    }

    QMetaObject::invokeMethod(
        window,
        [window, plan]() {
            QCheckBox* cb1 = window->findChild<QCheckBox*>("checkBox_1");
            QCheckBox* cb2 = window->findChild<QCheckBox*>("checkBox_2");
            QCheckBox* cb3 = window->findChild<QCheckBox*>("checkBox_3");

            if (!cb1 || !cb2 || !cb3) {
                return;
            }

            QSignalBlocker blocker1(cb1);
            QSignalBlocker blocker2(cb2);
            QSignalBlocker blocker3(cb3);

            cb1->setChecked(plan == 1);
            cb2->setChecked(plan == 2);
            cb3->setChecked(plan == 3);
        },
        Qt::QueuedConnection);
}


void TaskTrayApp::ShowControlPanel() {
    if (auto window = g_controlPanelWindow.load()) {
        const bool isRunning = g_controlPanelRunning.load();
        QCoreApplication* appInstance = QCoreApplication::instance();
        if (!isRunning || appInstance == nullptr || QCoreApplication::closingDown()) {
            QMainWindow* expectedWindow = window;
            if (g_controlPanelWindow.compare_exchange_strong(expectedWindow, nullptr)) {
                DebugLog("ShowControlPanel: Cleared stale control panel window before relaunch.");
            }
        }
        else {
            QMetaObject::invokeMethod(window, [window]() {
                window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
                window->show();
                window->raise();
                window->activateWindow();
            }, Qt::QueuedConnection);
            DebugLog("ShowControlPanel: Control panel already running. Bringing window to front.");
            return;
        }
    }

    bool expected = false;
    if (!g_controlPanelRunning.compare_exchange_strong(expected, true)) {
        DebugLog("ShowControlPanel: Control panel launch already in progress. No new window created.");
        return;
    }

    std::uint64_t token = g_controlPanelToken.fetch_add(1) + 1;

    DebugLog("ShowControlPanel: Launching control panel UI.");

    std::thread([token]() {
        // 高DPIスケーリングを無効化（実ピクセルサイズで表示�E�E
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
        qputenv("QT_SCALE_FACTOR", "1");
        
        int argc = 0;
        char* argv[] = { nullptr };
        QApplication app(argc, argv);
        QApplication::setQuitOnLastWindowClosed(false);

        auto mainWindow = std::make_unique<QMainWindow>();
        Ui_MainWindow ui;
        ui.setupUi(mainWindow.get());

        // -------- Activation / ServerName persistence --------
        ServerActivationConfig cfg;
        cfg.machineId = GetMachineId();
        cfg.lanIp = GetLanIpv4();
        LoadServerConfig(cfg); // best-effort (if decryption fails, keeps defaults)

        auto refreshActivationUi = [&ui, &cfg]() {
            if (ui.textEdit_0) {
                ui.textEdit_0->setPlainText(QString::fromUtf8(cfg.serverName.c_str()));
            }
            if (ui.textEdit_1) ui.textEdit_1->setPlainText(QString::fromUtf8(cfg.userEmail.c_str()));
            if (ui.textEdit_2) ui.textEdit_2->setPlainText(QString::fromUtf8(cfg.password.c_str()));
            if (ui.textEdit_3) ui.textEdit_3->setPlainText(QString::fromUtf8(cfg.activationCode.c_str()));

            const bool hasSavedServerName = !Trim(cfg.serverName).empty();
            bool uiMatchesSaved = true;
            if (ui.textEdit_0) {
                const std::string uiName = Trim(ui.textEdit_0->toPlainText().toUtf8().toStdString());
                uiMatchesSaved = (uiName == Trim(cfg.serverName));
            }
            const bool canActivate = hasSavedServerName && uiMatchesSaved;

            // Activation tab gating: cannot switch to Settings until ServerName saved and activated
            if (ui.tabWidget) {
                // Settings tab index = 1
                ui.tabWidget->setTabEnabled(1, cfg.activated);
            }

            if (ui.pushButton_1) ui.pushButton_1->setEnabled(canActivate);

            if (ui.label_06) {
                ui.label_06->setText(cfg.activated ? "Activated" : "Unactivated");
            }
            if (ui.label_07) {
                ui.label_07->setText(cfg.activated ? "" : "");
            }
        };

        refreshActivationUi();

        if (ui.tabWidget) {
            // Initial tab: System. After activation, default to Settings.
            ui.tabWidget->setCurrentIndex(cfg.activated ? 1 : 0);
            QObject::connect(ui.tabWidget, &QTabWidget::currentChanged, [&, oldIdx = 0](int idx) mutable {
                // Prevent switching to Settings unless activated.
                if (idx == 1 && !cfg.activated) {
                    if (ui.tabWidget) {
                        ui.tabWidget->blockSignals(true);
                        ui.tabWidget->setCurrentIndex(0);
                        ui.tabWidget->blockSignals(false);
                    }
                }
                oldIdx = idx;
            });
        }

        // Save ServerName button
        if (ui.pushButton_0) {
            QObject::connect(ui.pushButton_0, &QPushButton::clicked, [&, refreshActivationUi]() {
                if (!ui.textEdit_0) return;
                std::string newName = Trim(ui.textEdit_0->toPlainText().toUtf8().toStdString());
                if (newName.empty()) {
                    DebugLog("ControlPanel(System): ServerName is empty; refusing to save.");
                    return;
                }
                bool changed = (newName != cfg.serverName);
                cfg.serverName = newName;
                cfg.machineId = GetMachineId();
                cfg.lanIp = GetLanIpv4();

                // If not activated yet, keep secrets empty.
                if (!cfg.activated) {
                    ClearActivationSecrets(cfg);
                }

                if (!SaveServerConfig(cfg)) {
                    DebugLog("ControlPanel(System): Failed to save encrypted Server config.");
                }

                // If activated and name changed, update DB device_name (no reactivation)
                if (cfg.activated && changed && !cfg.userEmail.empty() && !cfg.password.empty() && !cfg.activationCode.empty()) {
                    std::wstring baseUrl = GetEnvW(L"HAYATEKOMOREBI_APP_URL", L"https://myapp.local");
                    std::string body = std::string("{")
                        + "\"email\":\"" + JsonEscape(cfg.userEmail) + "\"," 
                        + "\"password\":\"" + JsonEscape(cfg.password) + "\"," 
                        + "\"activation_code\":\"" + JsonEscape(cfg.activationCode) + "\"," 
                        + "\"role\":\"server\"," 
                        + "\"machine_id\":\"" + JsonEscape(cfg.machineId) + "\"," 
                        + "\"device_name\":\"" + JsonEscape(cfg.serverName) + "\"," 
                        + "\"lan_ip\":\"" + JsonEscape(cfg.lanIp) + "\"";
                    body += "}";
                    std::string resp;
                    if (!WinHttpPostJson(baseUrl, L"/api/device_update_name.php", body, resp)) {
                        DebugLog("ControlPanel(System): device_update_name POST failed.");
                    }
                }

                refreshActivationUi();
            });
        }

        // Activate button
        if (ui.pushButton_1) {
            QObject::connect(ui.pushButton_1, &QPushButton::clicked, [&, refreshActivationUi]() {
                if (!ui.textEdit_0 || !ui.textEdit_1 || !ui.textEdit_2 || !ui.textEdit_3) return;

                const std::string uiName = Trim(ui.textEdit_0->toPlainText().toUtf8().toStdString());
                if (uiName.empty() || uiName != Trim(cfg.serverName)) {
                    DebugLog("ControlPanel(System): Refusing to activate because ServerName is not saved (press Save first).");
                    refreshActivationUi(); return;
                }
                cfg.userEmail  = Trim(ui.textEdit_1->toPlainText().toUtf8().toStdString());
                cfg.password   = ui.textEdit_2->toPlainText().toUtf8().toStdString();
                cfg.activationCode = Trim(ui.textEdit_3->toPlainText().toUtf8().toStdString());
                cfg.machineId = GetMachineId();
                cfg.lanIp = GetLanIpv4();

                if (cfg.serverName.empty() || cfg.userEmail.empty() || cfg.password.empty() || cfg.activationCode.empty()) {
                    DebugLog("ControlPanel(System): Missing fields for activation.");
                    return;
                }

                // Save credentials immediately; activation status will be confirmed via polling.
                cfg.activated = false;
                if (!SaveServerConfig(cfg)) {
                    DebugLog("ControlPanel(System): Failed to save encrypted Server config before activation.");
                }

                std::wstring baseUrl = GetEnvW(L"HAYATEKOMOREBI_APP_URL", L"https://myapp.local");
                std::string publicKeyPem;
                (void)DeviceKeyCrypto::GetServerPublicKeyForDb(publicKeyPem);
                std::string body = std::string("{")
                    + "\"email\":\"" + JsonEscape(cfg.userEmail) + "\"," 
                    + "\"password\":\"" + JsonEscape(cfg.password) + "\"," 
                    + "\"activation_code\":\"" + JsonEscape(cfg.activationCode) + "\"," 
                    + "\"role\":\"server\"," 
                    + "\"machine_id\":\"" + JsonEscape(cfg.machineId) + "\"," 
                    + "\"device_name\":\"" + JsonEscape(cfg.serverName) + "\"," 
                    + "\"lan_ip\":\"" + JsonEscape(cfg.lanIp) + "\"";
                if (!publicKeyPem.empty()) body += ",\"public_key_pem\":\"" + JsonEscape(publicKeyPem) + "\"";
                body += "}";

                std::string resp;
                if (!WinHttpPostJson(baseUrl, L"/api/app_activation_start.php", body, resp)) {
                    DebugLog("ControlPanel(System): app_activation_start POST failed.");
                    return;
                }
                std::string url;
                if (!JsonExtractString(resp, "url", url) || url.empty()) {
                    DebugLog("ControlPanel(System): Failed to parse activation URL.");
                    return;
                }
                DebugLog(std::string("ControlPanel(System): Opening activation URL: ") + url);
                ShellExecuteW(nullptr, L"open", ToW(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

                refreshActivationUi();
            });
        }

        // NOTE: Activation validity polling is handled by TaskTrayApp's background thread.

        // チェチE��ボックスを排他的にする�E�Eつだけ選択可能�E�E
        QCheckBox* cb1 = ui.checkBox_1;
        QCheckBox* cb2 = ui.checkBox_2;
        QCheckBox* cb3 = ui.checkBox_3;
        QPushButton* saveButton = ui.pushButton_2;

        // 初期状態として、現在の OptimizedPlan に合わせてチェチE��を設宁E
        if (TaskTrayApp* appInst = g_taskTrayAppInstance.load()) {
            int plan = appInst->GetOptimizedPlanForSync();
            if (cb1 && cb2 && cb3) {
                cb1->setChecked(plan == 1);
                cb2->setChecked(plan == 2);
                cb3->setChecked(plan == 3);
            }
        }

        QObject::connect(cb1, &QCheckBox::toggled, [cb2, cb3](bool checked) {
            if (checked) {
                cb2->setChecked(false);
                cb3->setChecked(false);
            }
        });

        QObject::connect(cb2, &QCheckBox::toggled, [cb1, cb3](bool checked) {
            if (checked) {
                cb1->setChecked(false);
                cb3->setChecked(false);
            }
        });

        QObject::connect(cb3, &QCheckBox::toggled, [cb1, cb2](bool checked) {
            if (checked) {
                cb1->setChecked(false);
                cb2->setChecked(false);
            }
        });

        if (saveButton) {
            QObject::connect(saveButton, &QPushButton::clicked, [cb1, cb2, cb3]() {
                int plan = 0;
                if (cb1 && cb1->isChecked()) {
                    plan = 1;
                }
                else if (cb2 && cb2->isChecked()) {
                    plan = 2;
                }
                else if (cb3 && cb3->isChecked()) {
                    plan = 3;
                }

                if (plan == 0) {
                    DebugLog("ControlPanel: Save clicked but no speed mode selected.");
                    return;
                }

                TaskTrayApp* appInst = g_taskTrayAppInstance.load();
                if (!appInst) {
                    DebugLog("ControlPanel: g_taskTrayAppInstance is null on Save.");
                    return;
                }

                appInst->UpdateOptimizedPlanFromUi(plan);
            });
        }
        else {
            DebugLog("ControlPanel: Save button (pushButton_2) not found.");
        }

        QMainWindow* rawWindow = mainWindow.get();

        auto resetState = [token, rawWindow]() {
            bool pointerCleared = false;
            if (g_controlPanelWindow.load() == rawWindow) {
                g_controlPanelWindow.store(nullptr);
                pointerCleared = true;
                DebugLog("ShowControlPanel: Cleared control panel window pointer.");
            }

            if (g_controlPanelToken.load() == token) {
                if (g_controlPanelRunning.exchange(false)) {
                    DebugLog("ShowControlPanel: Cleared control panel running flag.");
                }
            }
            else if (pointerCleared) {
                DebugLog("ShowControlPanel: Window pointer cleared for stale control panel session.");
            }
        };

        struct CleanupGuard {
            std::function<void()> fn;
            ~CleanupGuard() { if (fn) fn(); }
        } cleanupGuard{ resetState };

        DebugLog("ShowControlPanel: Control panel window initialized.");

        g_controlPanelWindow.store(rawWindow);

        auto closeFilter = new ControlPanelCloseFilter(rawWindow);
        rawWindow->installEventFilter(closeFilter);

        QObject::connect(&app, &QApplication::aboutToQuit, [resetState]() {
            DebugLog("ShowControlPanel: QApplication about to quit.");
            resetState();
        });

        QObject::connect(rawWindow, &QObject::destroyed, [resetState]() {
            DebugLog("ShowControlPanel: Control panel window destroyed.");
            resetState();
        });

        rawWindow->show();
        DebugLog("ShowControlPanel: Entering Qt event loop for control panel.");
        app.exec();
        DebugLog("ShowControlPanel: Qt event loop exited.");
    }).detach();
}

LRESULT CALLBACK TaskTrayApp::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    TaskTrayApp* app = nullptr;
    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        app = reinterpret_cast<TaskTrayApp*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        // Initialize the OverlayManager singleton
        OverlayManager::Instance().Initialize(app->hInstance, hwnd);
    }
    else {
        app = reinterpret_cast<TaskTrayApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (app) {
        if (uMsg == WM_TASKBARCREATED) {
            DebugLog("WindowProc: TaskbarCreated message received. Re-creating icon.");
            app->CreateTrayIcon();
            // Fall-through to WM_USER+2 to also update the tooltip
        }

        switch (uMsg) {
        case WM_USER + 1: // Our custom message for tray icon events
            if (lParam == WM_RBUTTONUP) {
                DebugLog("WindowProc: Tray icon right-clicked.");
                app->ShowContextMenu();
            }
            break;

        case WM_USER + 2: // Custom message to refresh UI (e.g., after display change)
        {
            DebugLog("WindowProc: WM_USER + 2 - Refreshing UI.");
            SharedMemoryHelper sharedMemoryHelper; // No args
            std::string selectedSerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
            std::string numDisplaysStr = sharedMemoryHelper.ReadSharedMemory("DISP_INFO_NUM");
            int numDisplays = 0;
            if (!numDisplaysStr.empty()) {
                try { numDisplays = std::stoi(numDisplaysStr); }
                catch (...) {}
            }

            if (numDisplays > MAX_DISPLAY_MENU_ITEMS) {
                numDisplays = MAX_DISPLAY_MENU_ITEMS;
            }

            int selectedIndex = -1;
            for (int idx = 0; idx < numDisplays; ++idx) {
                std::string key = "DISP_INFO_" + std::to_string(idx);
                if (sharedMemoryHelper.ReadSharedMemory(key) == selectedSerial) {
                    selectedIndex = idx + 1; // human-friendly 1-based label
                    break;
                }
            }

            if (selectedIndex != -1) {
                app->UpdateTrayTooltip(L"Display Manager - Selected: Display " + std::to_wstring(selectedIndex));
            }
            else if (numDisplays > 0) {
                app->UpdateTrayTooltip(L"Display Manager");
            }
            else {
                app->UpdateTrayTooltip(L"Display Manager - No displays");
            }
        }
        break;

        case WM_DISPLAYCHANGE:
            DebugLog("WindowProc: WM_DISPLAYCHANGE received. Posting UI refresh message.");
            // Hide any active overlays in case the menu is open during a display change.
            OverlayManager::Instance().HideAll();
            // Display configuration changed. Post a message to ourselves to update the UI.
            // The background thread will handle the logic, this just updates the tooltip.
            PostMessage(app->hwnd, WM_USER + 2, 0, 0);
            break;

        case WM_MENUSELECT:
        {
            UINT cmdId = LOWORD(wParam);
            UINT flags = HIWORD(wParam);

            if ((flags & MF_HILITE) && !(flags & MF_POPUP)) {
                if (cmdId >= ID_DISPLAY_BASE && cmdId < 200) { // Display items are in this range
                    int idx0 = (cmdId - ID_DISPLAY_BASE);      // 0-based index for shared memory
                    int overlayNumber = idx0 + 1;  // 1-based number for display label
                    SharedMemoryHelper smh; // No args
                    std::string key = "DISP_INFO_" + std::to_string(idx0);
                    std::string serial = smh.ReadSharedMemory(key);
                    if (!serial.empty()) {
                        OverlayManager::Instance().ShowNumberForSerial(overlayNumber, serial);
                    } else {
                        OverlayManager::Instance().HideAll();
                    }
                } else {
                    // Not a display item, hide the overlay
                    OverlayManager::Instance().HideAll();
                }
            }
            break;
        }

        case WM_EXITMENULOOP:
        case WM_UNINITMENUPOPUP:
            // Hide overlay when the menu is closed for any reason
            OverlayManager::Instance().HideAll();
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_EXIT) { // Exit command
                DebugLog("WindowProc: Exit command received.");
                if (app->Cleanup()) {
                    DebugLog("WindowProc: Cleanup succeeded.");
                    PostQuitMessage(0);
                }
                else {
                    DebugLog("WindowProc: Cleanup failed.");
                }
            }
            else if (LOWORD(wParam) >= ID_DISPLAY_BASE && LOWORD(wParam) < 200) { // Display selection
                DebugLog("WindowProc: Display selection command received.");
                app->SelectDisplay(LOWORD(wParam) - ID_DISPLAY_BASE);
            }
            else if (LOWORD(wParam) == ID_CAPTURE_MODE_NORMAL) {
                DebugLog("WindowProc: Normal Mode selected.");
                app->SetCaptureMode(1);
            }
            else if (LOWORD(wParam) == ID_CAPTURE_MODE_GAME) {
                DebugLog("WindowProc: Game Mode selected.");
                app->SetCaptureMode(2);
            }
            else if (LOWORD(wParam) == ID_CONTROL_PANEL) {
                DebugLog("WindowProc: Control Panel selected.");
                app->ShowControlPanel();
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        default:
            if (uMsg != WM_TASKBARCREATED) {
                 return DefWindowProc(hwnd, uMsg, wParam, lParam);
            }
            break;
        }
    } else {
         return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}




bool TaskTrayApp::RefreshDisplayList() {
    // Only read from Shared Memory to update UI state (tooltip).
    DebugLog("RefreshDisplayList: Updating UI from Shared Memory.");

    SharedMemoryHelper sharedMemoryHelper; // No args
    std::string numDisplaysStr = sharedMemoryHelper.ReadSharedMemory("DISP_INFO_NUM");
    if (numDisplaysStr.empty()) {
        DebugLog("RefreshDisplayList: Shared Memory not ready.");
        UpdateTrayTooltip(L"Display Manager - Service not ready");
        return false;
    }

    // Update tooltip based on current selection
    std::string selectedSerial = sharedMemoryHelper.ReadSharedMemory("DISP_INFO");
    int numDisplays = 0;
    try { numDisplays = std::stoi(numDisplaysStr); } catch (...) {}

    if (numDisplays > MAX_DISPLAY_MENU_ITEMS) {
        numDisplays = MAX_DISPLAY_MENU_ITEMS;
    }

    int selectedIndex = -1;
    for (int idx = 0; idx < numDisplays; ++idx) {
        std::string key = "DISP_INFO_" + std::to_string(idx);
        if (sharedMemoryHelper.ReadSharedMemory(key) == selectedSerial) {
            selectedIndex = idx + 1;
            break;
        }
    }

    if (selectedIndex != -1) {
        UpdateTrayTooltip(L"Display Manager - Selected: Display " + std::to_wstring(selectedIndex));
    } else if (numDisplays > 0) {
        UpdateTrayTooltip(L"Display Manager");
    } else {
        UpdateTrayTooltip(L"Display Manager - No displays");
    }

    return true;
}

int TaskTrayApp::Run() {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

void TaskTrayApp::UpdateTrayTooltip(const std::wstring& text) {
    DebugLog("UpdateTrayTooltip: Setting tooltip to: " + utf16_to_utf8(text));
    // Copy the new text to the tooltip member of the NOTIFYICONDATA struct.
    // lstrcpyn is a safe way to copy, preventing buffer overflows.
    lstrcpyn(nid.szTip, text.c_str(), _countof(nid.szTip));

    // The uFlags member must specify that the tip is being updated.
    // Also include NIF_INFO to make sure the tooltip is shown as a balloon notification if needed,
    // though here we are just updating the hover tip.
    nid.uFlags = NIF_TIP;

    // Modify the tray icon with the new tooltip.
    if (!Shell_NotifyIcon(NIM_MODIFY, &nid)) {
        DebugLog("UpdateTrayTooltip: Shell_NotifyIcon failed.");
    }
}

void TaskTrayApp::UpdateCaptureModeMenu(HMENU hMenu) {
    while (GetMenuItemCount(hMenu) > 0) {
        RemoveMenu(hMenu, 0, MF_BYPOSITION);
    }

    SharedMemoryHelper sharedMemoryHelper; // No args
    std::string captureModeStr = sharedMemoryHelper.ReadSharedMemory("Capture_Mode");
    int captureMode = 1;

    if (!captureModeStr.empty()) {
        try {
            captureMode = std::stoi(captureModeStr);
        }
        catch (...) {
            captureMode = 1;
        }
    }

    UINT normalFlags = MF_STRING;
    UINT gameFlags = MF_STRING;

    if (captureMode == 2) {
        gameFlags |= MF_CHECKED;
    }
    else {
        normalFlags |= MF_CHECKED;
    }

    AppendMenu(hMenu, normalFlags, ID_CAPTURE_MODE_NORMAL, _T("Normal Mode"));
    AppendMenu(hMenu, gameFlags, ID_CAPTURE_MODE_GAME, _T("Game Mode"));
}
