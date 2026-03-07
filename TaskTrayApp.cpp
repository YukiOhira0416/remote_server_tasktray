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
#include "DisplaySyncServer.h"
#include "ModeSyncServer.h"
#include "DeviceKeyCrypto.h"
#include "DeviceSignKey.h"
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
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include "ui_Main_UI.h"

#include <wincrypt.h>
#include <winhttp.h>
#include <iphlpapi.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")


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
std::unique_ptr<QApplication> g_qtApplication;
int g_qtArgc = 1;
char g_qtArg0[] = "remote_server_tasktray";
char* g_qtArgv[] = { g_qtArg0, nullptr };

static bool EnsureQtApplicationCreated() {
    if (auto* existing = QCoreApplication::instance()) {
        return qobject_cast<QApplication*>(existing) != nullptr;
    }

    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", "1");

    g_qtApplication = std::make_unique<QApplication>(g_qtArgc, g_qtArgv);
    QApplication::setQuitOnLastWindowClosed(false);
    DebugLog("EnsureQtApplicationCreated: QApplication created on tray main thread.");
    return true;
}

class ControlPanelCloseFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event && event->type() == QEvent::Close) {
            if (auto* window = qobject_cast<QWidget*>(watched)) {
                // The control panel should be closable with the titlebar [X].
                // Keep the instance alive (so the tray can reopen it quickly), just hide it.
                DebugLog("ControlPanelCloseFilter: Close requested. Hiding control panel window.");
                window->hide();
            }
            // Consume the close event so the window stays alive.
            event->ignore();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
};

// ServerActivationConfig (v2)
//
// IMPORTANT DESIGN CHANGES
// - Login / passkey / license updates are handled in the dashboard (website).
// - The tray app never asks for email/password/activation codes.
// - Device activation uses a short-lived pairing code created in the dashboard.
// - The tray app stores only opaque device session state:
//     device_id / refresh_token / license_blob
// - The device refresh request is authenticated with an Ed25519 signature.
//   The private key is stored in %ProgramData% protected by DPAPI (LocalMachine).
struct ServerActivationConfig {
    std::string serverName;

    // v2 device session state
    std::string deviceId;               // device_uuid
    std::string refreshToken;           // opaque refresh token (rotated)
    std::string licenseBlob;            // server-signed license blob
    std::string entitlementExpiresAt;   // ISO8601 (optional)
    std::string lastSuccessRefreshAt;   // ISO8601 local time (optional)
    bool activated = false;             // enrolled and usable (tokens present)
    bool licenseBlocked = false;        // server returned license_expired

    // derived device info
    std::string machineId;
    std::string lanIp;

    // Legacy fields (migration only; never written)
    std::string userEmail;
    std::string password;
    std::string activationCode;
    std::string activationExpiresOn;
};

static void EnsureTrayHostWindowHidden(HWND hwnd) {
    if (!hwnd) {
        return;
    }

    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);

    LONG_PTR desiredExStyle = exStyle | WS_EX_TOOLWINDOW;
    desiredExStyle &= ~WS_EX_APPWINDOW;
    desiredExStyle &= ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
    if (desiredExStyle != exStyle) {
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, desiredExStyle);
    }

    LONG_PTR desiredStyle = style & ~WS_VISIBLE;
    desiredStyle &= ~WS_OVERLAPPEDWINDOW;
    desiredStyle &= ~WS_CAPTION;
    desiredStyle &= ~WS_SYSMENU;
    desiredStyle &= ~WS_THICKFRAME;
    desiredStyle &= ~WS_MINIMIZEBOX;
    desiredStyle &= ~WS_MAXIMIZEBOX;
    desiredStyle |= WS_POPUP;
    if (desiredStyle != style) {
        SetWindowLongPtr(hwnd, GWL_STYLE, desiredStyle);
    }

    SetWindowPos(
        hwnd,
        HWND_BOTTOM,
        -32000,
        -32000,
        0,
        0,
        SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_FRAMECHANGED
    );
    ShowWindow(hwnd, SW_HIDE);
}

static void ForceForegroundWindow(HWND target) {
    if (!target) {
        return;
    }

    // Ensure the window is focusable.
    LONG_PTR exStyle = GetWindowLongPtr(target, GWL_EXSTYLE);
    if (exStyle & WS_EX_NOACTIVATE) {
        SetWindowLongPtr(target, GWL_EXSTYLE, exStyle & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
    }

    ShowWindow(target, SW_SHOWNORMAL);
    SetWindowPos(
        target,
        HWND_TOP,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

    // Robust foreground activation: attach input to the current foreground thread if needed.
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD thisThread = GetCurrentThreadId();
    if (fgThread && fgThread != thisThread) {
        AttachThreadInput(thisThread, fgThread, TRUE);
        SetForegroundWindow(target);
        SetActiveWindow(target);
        SetFocus(target);
        BringWindowToTop(target);
        AttachThreadInput(thisThread, fgThread, FALSE);
    }
    else {
        SetForegroundWindow(target);
        SetActiveWindow(target);
        SetFocus(target);
        BringWindowToTop(target);
    }
}

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

static std::string ExecCmdCaptureHidden(const std::wstring& cmdLine) {
    std::string out;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        DebugLog("ExecCmdCaptureHidden: CreatePipe failed.");
        return out;
    }

    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        DebugLog("ExecCmdCaptureHidden: SetHandleInformation failed.");
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return out;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine;
    if (!CreateProcessW(
            nullptr,
            mutableCmd.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        DebugLog("ExecCmdCaptureHidden: CreateProcessW failed.");
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return out;
    }

    CloseHandle(writePipe);
    writePipe = nullptr;

    char buffer[512];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr) && bytesRead > 0) {
        out.append(buffer, buffer + bytesRead);
    }

    WaitForSingleObject(pi.hProcess, 5000);

    CloseHandle(readPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out;
}

static std::string ExtractWmicValue(const std::string& text, const std::string& key) {
    auto pos = text.find(key + "=");
    if (pos != std::string::npos) {
        pos += key.size() + 1;
        auto end = text.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = text.size();
        return Trim(text.substr(pos, end - pos));
    }

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        if (line.find(key) != std::string::npos) continue;
        return line;
    }
    return "";
}

static std::string GetRegistryStringValueA(HKEY root, const char* subKey, const char* valueName) {
    char buffer[512] = {0};
    DWORD cb = sizeof(buffer);
    DWORD type = 0;
    if (RegGetValueA(root, subKey, valueName, RRF_RT_REG_SZ, &type, buffer, &cb) != ERROR_SUCCESS) {
        return "";
    }
    if (cb == 0) {
        return "";
    }
    buffer[sizeof(buffer) - 1] = '\0';
    return Trim(std::string(buffer));
}

static std::string GetLegacyMachineId() {
    std::string cpu = ExecCmdCaptureHidden(L"cmd.exe /d /c wmic cpu get ProcessorId /value 2>nul");
    std::string uuid = ExecCmdCaptureHidden(L"cmd.exe /d /c wmic csproduct get UUID /value 2>nul");

    std::string cpuId = ExtractWmicValue(cpu, "ProcessorId");
    std::string sysUuid = ExtractWmicValue(uuid, "UUID");
    if (cpuId.empty()) cpuId = "UNKNOWNCPU";
    if (sysUuid.empty()) sysUuid = "UNKNOWNUUID";
    return cpuId + "-" + sysUuid;
}

static bool IsPlaceholderMachineId(const std::string& value) {
    const std::string trimmed = Trim(value);
    return trimmed.empty()
        || trimmed == "UNKNOWNCPU-UNKNOWNUUID"
        || trimmed == "UNKNOWNCPU"
        || trimmed == "UNKNOWNUUID";
}

static std::string HexEncodeUpper(uint64_t value) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

static std::string Fnv1a64Hex(const std::string& text) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : text) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return HexEncodeUpper(hash);
}

static std::string GetPrimaryMacAddress() {
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &bufLen);
    if (bufLen == 0) return "";
    std::vector<unsigned char> buf(bufLen);
    IP_ADAPTER_ADDRESSES* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &bufLen) != NO_ERROR) {
        return "";
    }

    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->PhysicalAddressLength == 0) continue;
        std::ostringstream oss;
        for (ULONG i = 0; i < a->PhysicalAddressLength; ++i) {
            if (i) oss << ':';
            oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(a->PhysicalAddress[i]);
        }
        return oss.str();
    }
    return "";
}

static std::string GetStableFallbackMachineSeed() {
    std::vector<std::string> parts;

    const std::string machineGuid = GetRegistryStringValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid");
    if (!machineGuid.empty()) parts.push_back("MachineGuid=" + machineGuid);

    const std::string sqmcMachineId = GetRegistryStringValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\SQMClient", "MachineId");
    if (!sqmcMachineId.empty()) parts.push_back("SqmMachineId=" + sqmcMachineId);

    const std::string baseBoard = ExtractWmicValue(ExecCmdCaptureHidden(L"cmd.exe /d /c wmic baseboard get SerialNumber /value 2>nul"), "SerialNumber");
    if (!baseBoard.empty()) parts.push_back("BaseBoard=" + baseBoard);

    const std::string biosSerial = ExtractWmicValue(ExecCmdCaptureHidden(L"cmd.exe /d /c wmic bios get SerialNumber /value 2>nul"), "SerialNumber");
    if (!biosSerial.empty()) parts.push_back("BiosSerial=" + biosSerial);

    char computerName[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD computerNameLen = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(computerName, &computerNameLen) && computerName[0] != '\0') {
        parts.push_back(std::string("ComputerName=") + computerName);
    }

    const std::string mac = GetPrimaryMacAddress();
    if (!mac.empty()) parts.push_back("Mac=" + mac);

    char windowsDir[MAX_PATH] = {0};
    if (GetWindowsDirectoryA(windowsDir, MAX_PATH) > 0) {
        char volumeRoot[MAX_PATH] = {0};
        strncpy_s(volumeRoot, windowsDir, 3);
        DWORD volumeSerial = 0;
        if (GetVolumeInformationA(volumeRoot, nullptr, 0, &volumeSerial, nullptr, nullptr, nullptr, 0)) {
            parts.push_back("VolumeSerial=" + HexEncodeUpper(volumeSerial));
        }
    }

    if (parts.empty()) {
        parts.push_back("Fallback=HayateKomorebi");
    }

    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) oss << '|';
        oss << parts[i];
    }
    return oss.str();
}

static std::string GetMachineId() {
    const std::string legacy = GetLegacyMachineId();
    if (!IsPlaceholderMachineId(legacy)) {
        return legacy;
    }

    const std::string seed = GetStableFallbackMachineSeed();
    const std::string derived = "MID-" + Fnv1a64Hex(seed);
    DebugLog(std::string("GetMachineId: using fallback machine id ") + derived + " because CPU/UUID lookup was unavailable.");
    return derived;
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
    // If NOT activated and all v2 secrets are cleared, keep only ServerName in the file.
    const bool secretsEmpty =
        c.deviceId.empty() &&
        c.refreshToken.empty() &&
        c.licenseBlob.empty() &&
        c.entitlementExpiresAt.empty() &&
        c.lastSuccessRefreshAt.empty() &&
        c.machineId.empty() &&
        c.lanIp.empty();

    if (!c.activated && secretsEmpty) {
        return oss.str();
    }

    if (!c.deviceId.empty())             oss << "DeviceId=" << c.deviceId << "\n";
    if (!c.refreshToken.empty())         oss << "RefreshToken=" << c.refreshToken << "\n";
    if (!c.licenseBlob.empty())          oss << "LicenseBlob=" << c.licenseBlob << "\n";
    if (!c.entitlementExpiresAt.empty()) oss << "EntitlementExpiresAt=" << c.entitlementExpiresAt << "\n";
    if (!c.lastSuccessRefreshAt.empty()) oss << "LastSuccessRefreshAt=" << c.lastSuccessRefreshAt << "\n";
    if (!c.machineId.empty())            oss << "MachineId=" << c.machineId << "\n";
    if (!c.lanIp.empty())                oss << "LanIp=" << c.lanIp << "\n";
    oss << "LicenseBlocked=" << (c.licenseBlocked ? "1" : "0") << "\n";

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
        else if (k == "DeviceId") c.deviceId = v;
        else if (k == "RefreshToken") c.refreshToken = v;
        else if (k == "LicenseBlob") c.licenseBlob = v;
        else if (k == "EntitlementExpiresAt") c.entitlementExpiresAt = v;
        else if (k == "LastSuccessRefreshAt") c.lastSuccessRefreshAt = v;
        else if (k == "MachineId") c.machineId = v;
        else if (k == "LanIp") c.lanIp = v;
        else if (k == "LicenseBlocked") c.licenseBlocked = (v == "1");
        else if (k == "Activated") c.activated = (v == "1");
        // Legacy (migration)
        else if (k == "UserEmail") c.userEmail = v;
        else if (k == "Password") c.password = v;
        else if (k == "ActivationCode") c.activationCode = v;
        else if (k == "ActivationExpiresOn") c.activationExpiresOn = v;
    }
    return c;
}

static bool SaveServerConfig(const ServerActivationConfig& cfg); // forward declaration

static bool LoadServerConfig(ServerActivationConfig& out) {
    auto path = GetServerConfigPath();
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::string cipher((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (cipher.empty()) return false;

    // Decrypt using the preferred hardware-derived MachineId, but also accept the legacy
    // UNKNOWNCPU-UNKNOWNUUID-based entropy so existing installs can be migrated in place.
    const std::string preferredMachineId = GetMachineId();
    const std::string legacyMachineId = GetLegacyMachineId();
    std::string plain;
    if (!DpapiDecrypt(cipher, preferredMachineId, plain)) {
        if (legacyMachineId == preferredMachineId || !DpapiDecrypt(cipher, legacyMachineId, plain)) {
            return false;
        }
    }

    out = ParseConfig(plain);

    // One-time migration: if legacy activation fields exist, wipe them.
    // The old activation scheme is no longer supported; user must re-enroll via pairing code.
    if (!out.userEmail.empty() || !out.password.empty() || !out.activationCode.empty() || !out.activationExpiresOn.empty()) {
        DebugLog("LoadServerConfig: legacy activation fields detected. Clearing and requiring re-enroll.");
        out.userEmail.clear();
        out.password.clear();
        out.activationCode.clear();
        out.activationExpiresOn.clear();
        out.deviceId.clear();
        out.refreshToken.clear();
        out.licenseBlob.clear();
        out.entitlementExpiresAt.clear();
        out.lastSuccessRefreshAt.clear();
        out.licenseBlocked = false;
        out.activated = false;
        // Persist the migrated state best-effort.
        (void)SaveServerConfig(out);
    }

    // Ensure derived fields exist even when config file is minimized.
    if (out.machineId.empty() || IsPlaceholderMachineId(out.machineId)) out.machineId = preferredMachineId;
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

static std::wstring GetAppBaseUrlW() {
    std::wstring url = GetEnvW(L"HAYATEKOMOREBI_APP_URL", L"https://software.hayatekomorebi.com");
    if (url.empty()) {
        return L"https://software.hayatekomorebi.com";
    }
    while (!url.empty() && url.back() == L'/') {
        url.pop_back();
    }
    if (url.find(L"://") == std::wstring::npos) {
        url = L"https://" + url;
    }
    return url.empty() ? L"https://software.hayatekomorebi.com" : url;
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
        if (err != ERROR_SERVICE_NOT_ACTIVE && err != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
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

struct ServiceRuntimeState {
    DWORD currentState = SERVICE_STOPPED;
    DWORD startType = SERVICE_DEMAND_START;
    bool exists = false;
};

static bool QueryServiceRuntimeState(const std::wstring& serviceName, ServiceRuntimeState& outState) {
    outState = {};

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        DebugLog("ServiceControl: OpenSCManager failed (query runtime).");
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) {
        DebugLog("ServiceControl: OpenService failed (query runtime).");
        CloseServiceHandle(scm);
        return false;
    }

    outState.exists = true;

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytesNeeded = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytesNeeded)) {
        DebugLog("ServiceControl: QueryServiceStatusEx failed (query runtime).");
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }
    outState.currentState = ssp.dwCurrentState;

    DWORD cfgBytesNeeded = 0;
    (void)QueryServiceConfigW(svc, nullptr, 0, &cfgBytesNeeded);
    if (cfgBytesNeeded != 0) {
        std::vector<BYTE> cfgBuf(cfgBytesNeeded);
        QUERY_SERVICE_CONFIGW* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
        if (QueryServiceConfigW(svc, cfg, cfgBytesNeeded, &cfgBytesNeeded)) {
            outState.startType = cfg->dwStartType;
        } else {
            DebugLog("ServiceControl: QueryServiceConfig failed (query runtime).");
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

enum class ManagedServicePolicy {
    InstallState,
    StandbyState
};

static const char* ManagedServicePolicyToString(ManagedServicePolicy policy) {
    switch (policy) {
    case ManagedServicePolicy::InstallState:
        return "install-state";
    case ManagedServicePolicy::StandbyState:
        return "standby-state";
    default:
        return "unknown";
    }
}

static std::mutex g_servicePolicyApplyMutex;

static bool EnforceManagedServicePolicy(const std::wstring& serviceName, ManagedServicePolicy policy, bool logOnlyOnChange) {
    std::lock_guard<std::mutex> guard(g_servicePolicyApplyMutex);

    ServiceRuntimeState state{};
    bool queried = QueryServiceRuntimeState(serviceName, state);
    if (!queried) {
        DebugLog(std::string("ServiceControl: Failed to query runtime state before enforcing policy: ") + ManagedServicePolicyToString(policy));
    }

    const DWORD desiredStartType = (policy == ManagedServicePolicy::InstallState) ? SERVICE_DEMAND_START : SERVICE_AUTO_START;
    const bool shouldBeRunning = (policy == ManagedServicePolicy::StandbyState);

    const bool startTypeMismatch = !queried || state.startType != desiredStartType;
    const bool runningMismatch = !queried || ((state.currentState == SERVICE_RUNNING) != shouldBeRunning);

    if (logOnlyOnChange && !startTypeMismatch && !runningMismatch) {
        return true;
    }

    DebugLog(std::string("ServiceControl: Enforcing policy=") + ManagedServicePolicyToString(policy)
        + ", currentState=" + std::to_string(queried ? state.currentState : 0)
        + ", startType=" + std::to_string(queried ? state.startType : 0));

    bool ok = true;
    if (policy == ManagedServicePolicy::InstallState) {
        if (!StopServiceIfRunning(serviceName)) {
            ok = false;
        }
        if (!SetServiceStartType(serviceName, SERVICE_DEMAND_START)) {
            ok = false;
        }
    } else {
        if (!SetServiceStartType(serviceName, SERVICE_AUTO_START)) {
            ok = false;
        }
        if (!StartServiceIfNeeded(serviceName)) {
            ok = false;
        }
    }

    if (!ok) {
        DebugLog(std::string("ServiceControl: Failed to fully enforce policy=") + ManagedServicePolicyToString(policy));
    }
    return ok;
}

static ManagedServicePolicy DetermineManagedServicePolicy(const ServerActivationConfig& cfg) {
    const bool usable =
        cfg.activated &&
        !cfg.licenseBlocked &&
        !cfg.serverName.empty() &&
        !cfg.deviceId.empty() &&
        !cfg.refreshToken.empty();

    return usable ? ManagedServicePolicy::StandbyState : ManagedServicePolicy::InstallState;
}

static std::string Win32ErrorToString(DWORD err) {
    LPSTR msg = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD n = FormatMessageA(flags, nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msg, 0, nullptr);
    std::string s;
    if (n && msg) {
        s.assign(msg, n);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
            s.pop_back();
        }
    } else {
        s = "Unknown error";
    }
    if (msg) LocalFree(msg);
    return s;
}

static void DebugLogWinHttpFailure(const char* stage) {
    const DWORD err = GetLastError();
    DebugLog(std::string(stage) + " failed. GetLastError=" + std::to_string(err) + " (" + Win32ErrorToString(err) + ")");
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
        DebugLogWinHttpFailure("WinHttpCrackUrl");
        return false;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    INTERNET_PORT port = uc.nPort;
    bool isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"HayateKomorebi/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        DebugLogWinHttpFailure("WinHttpOpen");
        return false;
    }

    const int timeoutMs = 15000;
    if (!WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs)) {
        DebugLogWinHttpFailure("WinHttpSetTimeouts");
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        DebugLogWinHttpFailure("WinHttpConnect");
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (isHttps) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        DebugLogWinHttpFailure("WinHttpOpenRequest");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    const wchar_t* hdrs = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    BOOL b = WinHttpSendRequest(hRequest, hdrs, (DWORD)-1L, (LPVOID)jsonBody.data(), (DWORD)jsonBody.size(), (DWORD)jsonBody.size(), 0);
    if (!b) {
        DebugLogWinHttpFailure("WinHttpSendRequest");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    b = WinHttpReceiveResponse(hRequest, nullptr);
    if (!b) {
        DebugLogWinHttpFailure("WinHttpReceiveResponse");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
        DebugLogWinHttpFailure("WinHttpQueryHeaders(STATUS_CODE)");
        statusCode = 0;
    }

    DWORD avail = 0;
    bool readOk = true;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            DebugLogWinHttpFailure("WinHttpQueryDataAvailable");
            readOk = false;
            break;
        }
        if (!avail) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) {
            DebugLogWinHttpFailure("WinHttpReadData");
            readOk = false;
            break;
        }
        chunk.resize(read);
        outResponse += chunk;
    } while (avail > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!readOk) {
        return false;
    }
    if (statusCode < 200 || statusCode >= 300) {
        DebugLog(std::string("WinHttpPostJson: HTTP status ") + std::to_string(statusCode) + ", response=" + outResponse);
        return false;
    }
    if (outResponse.empty()) {
        DebugLog("WinHttpPostJson: HTTP response body was empty.");
        return false;
    }

    return true;
}

struct HttpJsonResult {
    bool transportOk = false;   // WinHTTP succeeded enough to get a status/body
    DWORD statusCode = 0;
    std::string body;
};

// Returns status code + body even on non-2xx responses.
static HttpJsonResult WinHttpPostJsonEx(const std::wstring& baseUrl, const std::wstring& path, const std::string& jsonBody) {
    HttpJsonResult r;
    r.body.clear();

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    std::wstring url = baseUrl;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpCrackUrl");
        return r;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    INTERNET_PORT port = uc.nPort;
    bool isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"HayateKomorebi/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpOpen");
        return r;
    }

    const int timeoutMs = 15000;
    (void)WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpConnect");
        WinHttpCloseHandle(hSession);
        return r;
    }

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (isHttps) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpOpenRequest");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return r;
    }

    const wchar_t* hdrs = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    BOOL b = WinHttpSendRequest(hRequest, hdrs, (DWORD)-1L, (LPVOID)jsonBody.data(), (DWORD)jsonBody.size(), (DWORD)jsonBody.size(), 0);
    if (!b) {
        DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpSendRequest");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return r;
    }

    b = WinHttpReceiveResponse(hRequest, nullptr);
    if (!b) {
        DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpReceiveResponse");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return r;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
        DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpQueryHeaders(STATUS_CODE)");
        statusCode = 0;
    }

    DWORD avail = 0;
    bool readOk = true;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpQueryDataAvailable");
            readOk = false;
            break;
        }
        if (!avail) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) {
            DebugLogWinHttpFailure("WinHttpPostJsonEx: WinHttpReadData");
            readOk = false;
            break;
        }
        chunk.resize(read);
        r.body += chunk;
    } while (avail > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!readOk) {
        return r;
    }
    r.transportOk = true;
    r.statusCode = statusCode;
    return r;
}

static std::string NowIsoLocal() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

static bool ShouldClearSessionForReason(const std::string& reason) {
    return reason == "refresh_invalid" ||
           reason == "refresh_expired" ||
           reason == "refresh_reuse" ||
           reason == "device_revoked" ||
           reason == "sig_invalid" ||
           reason == "user_not_found";
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

static bool OpenUrlInDefaultBrowser(const std::wstring& rawUrl) {
    std::wstring url = rawUrl;
    // trim whitespace
    auto ltrim = url.find_first_not_of(L" \t\r\n");
    if (ltrim == std::wstring::npos) {
        DebugLog("OpenUrlInDefaultBrowser: URL is empty.");
        return false;
    }
    auto rtrim = url.find_last_not_of(L" \t\r\n");
    url = url.substr(ltrim, rtrim - ltrim + 1);
    if (url.empty()) {
        DebugLog("OpenUrlInDefaultBrowser: URL is empty.");
        return false;
    }

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        DebugLog("OpenUrlInDefaultBrowser: WinHttpCrackUrl failed.");
        return false;
    }
    if (!(uc.nScheme == INTERNET_SCHEME_HTTP || uc.nScheme == INTERNET_SCHEME_HTTPS)) {
        DebugLog("OpenUrlInDefaultBrowser: Unsupported URL scheme.");
        return false;
    }

    const HINSTANCE hInst = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const INT_PTR rc = reinterpret_cast<INT_PTR>(hInst);
    if (rc > 32) {
        return true;
    }

    DebugLog("OpenUrlInDefaultBrowser: ShellExecuteW failed. Falling back to explorer.exe.");

    std::wstring cmdLine = L"explorer.exe \"" + url + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        DebugLog("OpenUrlInDefaultBrowser: explorer.exe fallback failed.");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void ClearActivationSecrets(ServerActivationConfig& cfg) {
    cfg.deviceId.clear();
    cfg.refreshToken.clear();
    cfg.licenseBlob.clear();
    cfg.entitlementExpiresAt.clear();
    cfg.lastSuccessRefreshAt.clear();
    cfg.lanIp.clear();
    cfg.machineId.clear();
    cfg.licenseBlocked = false;
    cfg.activated = false;

    // Legacy fields (defensive)
    cfg.userEmail.clear();
    cfg.password.clear();
    cfg.activationCode.clear();
    cfg.activationExpiresOn.clear();
}

static void ShowActivationMessageBox(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text) {
    QMessageBox box(parent);
    box.setIcon(icon);
    box.setWindowTitle(title);
    box.setText(text);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

static bool JsonContainsTrue(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    auto p = json.find(pat);
    if (p == std::string::npos) return false;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == '\t')) ++p;
    return json.compare(p, 4, "true") == 0;
}

static bool IsPrintableAscii(char ch) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    return uch >= 0x21 && uch <= 0x7E;
}

static std::string SanitizeServerName(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        if (!IsPrintableAscii(ch)) {
            continue;
        }
        if (out.size() >= 20) {
            break;
        }
        out.push_back(ch);
    }
    return out;
}

static bool IsValidServerName(const std::string& input) {
    if (input.empty() || input.size() > 20) {
        return false;
    }
    for (char ch : input) {
        if (!IsPrintableAscii(ch)) {
            return false;
        }
    }
    return true;
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

    // 既存�EログファイルをバチE�E��E�アチE�E�Eする
    if (std::filesystem::exists(logFilePath)) {
        // 現在の日時を取征E
        std::time_t t = std::time(nullptr);
        std::tm tm;
        localtime_s(&tm, &t);

        // 日付文字�Eを作�E
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d%H%M%S");
        std::string timestamp = oss.str();

        // バックアチE�E�Eファイル名を作�E
        std::string backupFileName = timestamp + "_debuglog_tasktray.log.back";
        std::filesystem::path backupFilePath = exePath / backupFileName;

        // ファイルをリネ�Eム
        std::filesystem::rename(logFilePath, backupFilePath);

        // バックアチE�E�Eファイルの数を確認し、Eつを趁E�E��E�る場合�E古ぁE�E��E�のから削除
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

        // 日付頁E�E��E�ソート（新しい頁E�E��E�E
        std::sort(backupFiles.begin(), backupFiles.end(), std::greater<std::filesystem::path>());

        // 5つより多い場合、古ぁE�E��E�ァイルを削除
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

    hwnd = CreateWindowEx(
        WS_EX_TOOLWINDOW,
        _T("TaskTrayClass"),
        _T("Task Tray App"),
        WS_POPUP,
        -32000,
        -32000,
        0,
        0,
        NULL,
        NULL,
        hInstance,
        this
    );

    if (!hwnd) return false;

    EnsureTrayHostWindowHidden(hwnd);

    CreateTrayIcon();

    // 念のためタスクバーや Alt+Tab に現れない完全非表示状態を維持する
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    // 初回チE�E��E�スプレイ惁E�E��E�取征E
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

    // While the tray app is running, keep the service under tray policy control.
    StartServicePolicyThread();

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

void TaskTrayApp::StartServicePolicyThread() {
    if (servicePolicyRunning.exchange(true)) {
        return;
    }
    servicePolicyThread = std::thread(&TaskTrayApp::ServicePolicyThreadProc, this);
}

void TaskTrayApp::StopServicePolicyThread() {
    if (!servicePolicyRunning.exchange(false)) {
        return;
    }
    servicePolicyCv.notify_all();
    if (servicePolicyThread.joinable()) {
        servicePolicyThread.join();
    }
}

void TaskTrayApp::ActivationPollThreadProc() {
    // Poll every 60 seconds.
    // - Uses v2 refresh protocol:
    //     device_nonce -> sign -> device_refresh
    // - If refresh token invalid/reused or device revoked => clear local session and require re-enroll.
    // - If license expired => keep session but block service until license is renewed in the dashboard.

    auto refreshOnce = [](ServerActivationConfig& cfg) {
        const std::wstring baseUrl = GetAppBaseUrlW();

        // 1) Get nonce
        const std::string nonceBody = std::string("{")
            + "\"device_id\":\"" + JsonEscape(cfg.deviceId) + "\","
            + "\"refresh_token\":\"" + JsonEscape(cfg.refreshToken) + "\""
            + "}";

        const HttpJsonResult nonceResp = WinHttpPostJsonEx(baseUrl, L"/api/device_nonce.php", nonceBody);
        if (!nonceResp.transportOk || nonceResp.body.empty()) {
            DebugLog("ActivationPoll(v2): device_nonce transport failed.");
            return; // offline / transient
        }

        if (nonceResp.statusCode < 200 || nonceResp.statusCode >= 300) {
            std::string reason;
            (void)JsonExtractString(nonceResp.body, "reason", reason);
            std::string err;
            (void)JsonExtractString(nonceResp.body, "error", err);

            if (reason == "license_expired") {
                cfg.licenseBlocked = true;
                std::string exp;
                if (JsonExtractString(nonceResp.body, "entitlement_expires_at", exp)) {
                    cfg.entitlementExpiresAt = exp;
                }
                (void)SaveServerConfig(cfg);
                DebugLog("ActivationPoll(v2): license_expired at nonce stage.");
                return;
            }

            if (ShouldClearSessionForReason(reason)) {
                DebugLog(std::string("ActivationPoll(v2): nonce failed reason=") + reason + " error=" + err + " -> clearing session");
                ClearActivationSecrets(cfg);
                (void)SaveServerConfig(cfg);
                const std::wstring svcName = GetServiceNameW();
                (void)EnforceManagedServicePolicy(svcName, ManagedServicePolicy::InstallState, false);
                return;
            }

            DebugLog(std::string("ActivationPoll(v2): nonce HTTP ") + std::to_string(nonceResp.statusCode) + " error=" + err);
            return;
        }

        std::string nonceId;
        std::string nonce;
        if (!JsonExtractString(nonceResp.body, "nonce_id", nonceId) || nonceId.empty() ||
            !JsonExtractString(nonceResp.body, "nonce", nonce) || nonce.empty()) {
            DebugLog("ActivationPoll(v2): nonce response parse failed.");
            return;
        }

        // 2) Sign message
        const std::string signMsg = std::string("myapp:refresh:v1|") + cfg.deviceId + "|" + nonceId + "|" + nonce;
        std::string sigB64;
        std::string signErr;
        if (!DeviceSignKey::SignDetachedBase64(signMsg, sigB64, &signErr)) {
            DebugLog(std::string("ActivationPoll(v2): SignDetachedBase64 failed: ") + signErr);
            return;
        }

        // 3) Refresh
        const std::string refreshBody = std::string("{")
            + "\"device_id\":\"" + JsonEscape(cfg.deviceId) + "\","
            + "\"refresh_token\":\"" + JsonEscape(cfg.refreshToken) + "\","
            + "\"nonce_id\":\"" + JsonEscape(nonceId) + "\","
            + "\"nonce\":\"" + JsonEscape(nonce) + "\","
            + "\"device_sig_b64\":\"" + JsonEscape(sigB64) + "\""
            + "}";

        const HttpJsonResult refreshResp = WinHttpPostJsonEx(baseUrl, L"/api/device_refresh.php", refreshBody);
        if (!refreshResp.transportOk || refreshResp.body.empty()) {
            DebugLog("ActivationPoll(v2): device_refresh transport failed.");
            return;
        }

        if (refreshResp.statusCode < 200 || refreshResp.statusCode >= 300) {
            std::string reason;
            (void)JsonExtractString(refreshResp.body, "reason", reason);
            std::string err;
            (void)JsonExtractString(refreshResp.body, "error", err);

            if (reason == "license_expired") {
                cfg.licenseBlocked = true;
                std::string exp;
                if (JsonExtractString(refreshResp.body, "entitlement_expires_at", exp)) {
                    cfg.entitlementExpiresAt = exp;
                }
                (void)SaveServerConfig(cfg);
                DebugLog("ActivationPoll(v2): license_expired at refresh stage.");
                return;
            }

            if (ShouldClearSessionForReason(reason)) {
                DebugLog(std::string("ActivationPoll(v2): refresh failed reason=") + reason + " error=" + err + " -> clearing session");
                ClearActivationSecrets(cfg);
                (void)SaveServerConfig(cfg);
                const std::wstring svcName = GetServiceNameW();
                (void)EnforceManagedServicePolicy(svcName, ManagedServicePolicy::InstallState, false);
                return;
            }

            DebugLog(std::string("ActivationPoll(v2): refresh HTTP ") + std::to_string(refreshResp.statusCode) + " error=" + err);
            return;
        }

        // Success: rotate refresh token and update license blob
        std::string newRefresh;
        std::string newLicense;
        std::string exp;
        if (JsonExtractString(refreshResp.body, "refresh_token", newRefresh) && !newRefresh.empty()) {
            cfg.refreshToken = newRefresh;
        }
        if (JsonExtractString(refreshResp.body, "license_blob", newLicense) && !newLicense.empty()) {
            cfg.licenseBlob = newLicense;
        }
        if (JsonExtractString(refreshResp.body, "entitlement_expires_at", exp)) {
            cfg.entitlementExpiresAt = exp;
        }
        cfg.lastSuccessRefreshAt = NowIsoLocal();
        cfg.licenseBlocked = false;
        cfg.activated = true;
        const std::string currentIp = GetLanIpv4();
        if (!currentIp.empty()) {
            cfg.lanIp = currentIp;
        }
        (void)SaveServerConfig(cfg);
        const std::wstring svcName = GetServiceNameW();
        (void)EnforceManagedServicePolicy(svcName, DetermineManagedServicePolicy(cfg), false);
    };

    // Refresh schedule: attempt at most every 10 minutes when activated.
    auto lastAttempt = std::chrono::steady_clock::now() - std::chrono::hours(24);

    while (activationPollRunning.load()) {
        ServerActivationConfig cfg;
        if (LoadServerConfig(cfg)) {
            // Only run refresh if enrolled.
            if (cfg.activated && !cfg.deviceId.empty() && !cfg.refreshToken.empty()) {
                auto now = std::chrono::steady_clock::now();
                if (now - lastAttempt > std::chrono::minutes(10)) {
                    lastAttempt = now;
                    refreshOnce(cfg);
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

void TaskTrayApp::ServicePolicyThreadProc() {
    // Poll more aggressively than activation validation so manual SCM changes are reverted quickly
    // while the tray app is alive.
    ManagedServicePolicy lastPolicy = ManagedServicePolicy::InstallState;
    bool hasLastPolicy = false;

    while (servicePolicyRunning.load()) {
        ServerActivationConfig cfg;
        if (LoadServerConfig(cfg)) {
            const ManagedServicePolicy policy = DetermineManagedServicePolicy(cfg);
            const bool policyChanged = !hasLastPolicy || policy != lastPolicy;
            const std::wstring svcName = GetServiceNameW();
            (void)EnforceManagedServicePolicy(svcName, policy, !policyChanged);
            lastPolicy = policy;
            hasLastPolicy = true;
        } else {
            const ManagedServicePolicy policy = ManagedServicePolicy::InstallState;
            const bool policyChanged = !hasLastPolicy || policy != lastPolicy;
            const std::wstring svcName = GetServiceNameW();
            (void)EnforceManagedServicePolicy(svcName, policy, !policyChanged);
            lastPolicy = policy;
            hasLastPolicy = true;
        }

        std::unique_lock<std::mutex> lk(servicePolicyMutex);
        servicePolicyCv.wait_for(lk, std::chrono::seconds(2), [this]() {
            return !servicePolicyRunning.load();
        });
    }
}

bool TaskTrayApp::Cleanup() {
    // 2回呼ばれても安�Eにする�E�E�E�Exitメニュー + WinMain後�E琁E�E��E�ど�E�E�E�E
    if (cleaned.exchange(true)) {
        return true;
    }

    // Stop background workers first.
    StopServicePolicyThread();
    StopActivationPollThread();

    // タスクトレイアイコンを削除
    Shell_NotifyIcon(NIM_DELETE, &nid);

    // スレチE�E��E�の停止を指示
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

    if (QMainWindow* window = g_controlPanelWindow.exchange(nullptr)) {
        DebugLog("TaskTrayApp::Cleanup: Releasing control panel window.");
        if (QCoreApplication::instance()) {
            QMetaObject::invokeMethod(window, [window]() {
                window->hide();
                window->deleteLater();
            }, Qt::QueuedConnection);
        }
    }
    g_controlPanelRunning.store(false);

    if (QCoreApplication::instance()) {
        QMetaObject::invokeMethod(QCoreApplication::instance(), []() {
            DebugLog("TaskTrayApp::Cleanup: Requesting QApplication quit.");
            QCoreApplication::quit();
        }, Qt::QueuedConnection);
    }

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

    EnsureTrayHostWindowHidden(hwnd);

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
    const UINT commandId = TrackPopupMenu(
        hMenu,
        TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
        pt.x,
        pt.y,
        0,
        hwnd,
        NULL);
    if (commandId != 0) {
        PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(commandId, 0), 0);
    }
    PostMessage(hwnd, WM_NULL, 0, 0);
    EnsureTrayHostWindowHidden(hwnd);

    DestroyMenu(hMenu);
    EnsureTrayHostWindowHidden(hwnd);
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

        // Menu label: use a stable "Display N".
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
        // REBOOT とレジストリ反映はサービス側に雁E�E��E�E�E��E�めE
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

    // 既に UI 上�EチェチE�E��E�ボックスは Save を押した時点で反映済みだが、E
    // 他経路から呼ばれた場合にも確実に UI が現在の plan を示すよぁE�E��E�しておく、E
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

bool TaskTrayApp::IsActivatedForSync() const {
    // Read-only activation check used by sync servers.
    // Activation state is maintained by ActivationPollThreadProc.
    ServerActivationConfig cfg;
    if (!LoadServerConfig(cfg)) {
        return false;
    }

    const bool activated =
        cfg.activated &&
        !cfg.licenseBlocked &&
        !cfg.serverName.empty() &&
        !cfg.deviceId.empty() &&
        !cfg.refreshToken.empty();

    return activated;
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
    if (!EnsureQtApplicationCreated()) {
        DebugLog("ShowControlPanel: QApplication is not available.");
        return;
    }

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

                if (HWND windowHandle = reinterpret_cast<HWND>(window->winId())) {
                    ForceForegroundWindow(windowHandle);
                }

                if (QWidget* focusTarget = window->findChild<QLineEdit*>("textEdit_0")) {
                    focusTarget->setFocus(Qt::ActiveWindowFocusReason);
                }
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

    DebugLog("ShowControlPanel: Launching control panel UI on tray main thread.");
    auto mainWindow = std::make_unique<QMainWindow>();
    QMainWindow* rawWindow = mainWindow.get();
    struct ControlPanelState {
        Ui_MainWindow ui;
        ServerActivationConfig cfg;
        bool highlightServerNameError = false;
        bool autoMovedToSettingsTab = false;
    };

    auto state = std::make_shared<ControlPanelState>();
    state->ui.setupUi(rawWindow);

    // タスクトレイメニューから開いた直後でも通常のトップレベルウインドウとして
    // アクティベーションされるように明示しておく。
    mainWindow->setWindowFlag(Qt::Window, true);
    mainWindow->setWindowModality(Qt::NonModal);
    mainWindow->setAttribute(Qt::WA_ShowWithoutActivating, false);
    mainWindow->setFocusPolicy(Qt::StrongFocus);

    auto prepareEditorForInteractiveInput = [](QLineEdit* editor) {
        if (!editor) {
            return;
        }
        editor->setEnabled(true);
        editor->setReadOnly(false);
        editor->setFocusPolicy(Qt::StrongFocus);
        editor->setAttribute(Qt::WA_InputMethodEnabled, true);
        editor->setContextMenuPolicy(Qt::DefaultContextMenu);
    };

    prepareEditorForInteractiveInput(state->ui.textEdit_0); // ServerName
    prepareEditorForInteractiveInput(state->ui.textEdit_1); // PairingCode

    // v2: no email/password/activation code fields in the tray app.
    if (state->ui.textEdit_2) state->ui.textEdit_2->setVisible(false);
    if (state->ui.textEdit_3) state->ui.textEdit_3->setVisible(false);
    if (state->ui.label_03) state->ui.label_03->setVisible(false);
    if (state->ui.label_04) state->ui.label_04->setVisible(false);

    // -------- Activation / ServerName persistence --------
    state->cfg.machineId = GetMachineId();
    state->cfg.lanIp = GetLanIpv4();
    LoadServerConfig(state->cfg); // best-effort (if decryption fails, keeps defaults)

    auto setLineStyle = [](QWidget* w, const QString& style) {
        if (w) {
            w->setStyleSheet(style);
        }
    };

    auto sanitizeServerNameEditor = [state]() {
        if (!state->ui.textEdit_0) {
            return;
        }
        const QString original = state->ui.textEdit_0->text();
        const std::string sanitized = SanitizeServerName(original.toUtf8().toStdString());
        const QString qSanitized = QString::fromUtf8(sanitized.c_str());
        if (original != qSanitized) {
            const QSignalBlocker blocker(state->ui.textEdit_0);
            state->ui.textEdit_0->setText(qSanitized);
            state->ui.textEdit_0->setCursorPosition(qSanitized.size());
        }
    };

    auto setEditorTextIfNeeded = [](QLineEdit* editor, const QString& value, bool force) {
        if (!editor) {
            return;
        }
        if (!force) {
            if (editor->hasFocus()) {
                return;
            }
            if (editor->isModified()) {
                return;
            }
        }
        if (editor->text() == value) {
            return;
        }
        const QSignalBlocker blocker(editor);
        editor->setText(value);
        editor->setModified(false);
    };

    auto syncActivationEditorsFromConfig = [state, setEditorTextIfNeeded](bool force) {
        setEditorTextIfNeeded(state->ui.textEdit_0, QString::fromUtf8(state->cfg.serverName.c_str()), force);
        // Pairing code should never be persisted; clear it when enrolled.
        if (state->cfg.activated) {
            setEditorTextIfNeeded(state->ui.textEdit_1, QString(), force);
        }
    };

    auto refreshActivationUi = [state, setLineStyle, syncActivationEditorsFromConfig]() {
        syncActivationEditorsFromConfig(false);

        const bool hasSavedServerName = !Trim(state->cfg.serverName).empty();
        const bool savedServerNameValid = IsValidServerName(Trim(state->cfg.serverName));
        bool uiMatchesSaved = true;
        std::string uiName;
        if (state->ui.textEdit_0) {
            uiName = Trim(state->ui.textEdit_0->text().toUtf8().toStdString());
            uiMatchesSaved = (uiName == Trim(state->cfg.serverName));
        }
        const bool canActivate =
            !state->cfg.activated &&
            hasSavedServerName &&
            savedServerNameValid &&
            uiMatchesSaved &&
            state->ui.textEdit_1 &&
            !Trim(state->ui.textEdit_1->text().toUtf8().toStdString()).empty();

        // Activation tab gating: cannot switch to Settings until ServerName saved and enrolled
        if (state->ui.tabWidget) {
            // Settings tab index = 1
            const bool canUseSettings = state->cfg.activated && !state->cfg.licenseBlocked;
            state->ui.tabWidget->setTabEnabled(1, canUseSettings);
            if (canUseSettings && !state->autoMovedToSettingsTab && state->ui.tabWidget->currentIndex() == 0) {
                state->autoMovedToSettingsTab = true;
                state->ui.tabWidget->setCurrentIndex(1);
            }
            if (!canUseSettings) {
                state->autoMovedToSettingsTab = false;
            }
        }

        if (state->ui.pushButton_1) state->ui.pushButton_1->setEnabled(canActivate);

        if (state->ui.textEdit_1) {
            state->ui.textEdit_1->setEnabled(!state->cfg.activated);
        }

        if (state->ui.pushButton_0) {
            state->ui.pushButton_0->setStyleSheet(
                "QPushButton {"
                " background-color: #1f6feb;"
                " color: white;"
                " border: 1px solid #0d419d;"
                " border-radius: 6px;"
                " font-weight: 600;"
                " padding: 6px 12px;"
                "}"
                "QPushButton:disabled {"
                " background-color: #8aa4cf;"
                " color: #eef4ff;"
                "}"
            );
        }
        if (state->ui.pushButton_1) {
            state->ui.pushButton_1->setStyleSheet(
                "QPushButton {"
                " background-color: #1a7f37;"
                " color: white;"
                " border: 1px solid #115c28;"
                " border-radius: 6px;"
                " font-weight: 700;"
                " padding: 6px 12px;"
                "}"
                "QPushButton:disabled {"
                " background-color: #94b89e;"
                " color: #f4fff6;"
                "}"
            );
        }

        const bool markServerNameRequired = state->highlightServerNameError || !savedServerNameValid;
        setLineStyle(
            state->ui.textEdit_0,
            markServerNameRequired
                ? "QLineEdit { border: 2px solid #d93025; border-radius: 6px; padding: 4px; background: #fff5f5; }"
                : "QLineEdit { border: 1px solid #9aa4b2; border-radius: 6px; padding: 4px; background: white; }"
        );
        setLineStyle(
            state->ui.pushButton_0,
            markServerNameRequired
                ? "QPushButton { background-color: #d93025; color: white; border: 1px solid #a61b13; border-radius: 6px; font-weight: 700; padding: 6px 12px; }"
                  "QPushButton:disabled { background-color: #e7a29d; color: white; }"
                : "QPushButton { background-color: #1f6feb; color: white; border: 1px solid #0d419d; border-radius: 6px; font-weight: 600; padding: 6px 12px; }"
                  "QPushButton:disabled { background-color: #8aa4cf; color: #eef4ff; }"
        );

        if (state->ui.label_06) {
            if (!state->cfg.activated) {
                state->ui.label_06->setText("Not activated");
                state->ui.label_06->setStyleSheet("color:#d93025;font-weight:700;font-size:12px;");
            } else if (state->cfg.licenseBlocked) {
                state->ui.label_06->setText("License expired");
                state->ui.label_06->setStyleSheet("color:#d93025;font-weight:700;font-size:12px;");
            } else {
                state->ui.label_06->setText("Activated");
                state->ui.label_06->setStyleSheet("color:#1a7f37;font-weight:700;font-size:12px;");
            }
        }
        if (state->ui.label_07) {
            QString msg;
            if (state->cfg.activated) {
                if (!state->cfg.entitlementExpiresAt.empty()) {
                    msg += QString("License: %1%2")
                        .arg(state->cfg.licenseBlocked ? "expired at " : "until ")
                        .arg(QString::fromUtf8(state->cfg.entitlementExpiresAt.c_str()));
                }
                if (!state->cfg.lastSuccessRefreshAt.empty()) {
                    if (!msg.isEmpty()) msg += " | ";
                    msg += QString("Last check: %1").arg(QString::fromUtf8(state->cfg.lastSuccessRefreshAt.c_str()));
                }
                if (!state->cfg.deviceId.empty()) {
                    if (!msg.isEmpty()) msg += " | ";
                    msg += QString("Device: %1").arg(QString::fromUtf8(state->cfg.deviceId.c_str()).left(12));
                }
            }
            state->ui.label_07->setText(msg);
        }
        if (state->ui.label_05) {
            state->ui.label_05->setText("Use 1-20 printable ASCII characters.");
            state->ui.label_05->setStyleSheet("color:#5f6b7a;font-size:11px;");
        }
    };

    syncActivationEditorsFromConfig(true);
    refreshActivationUi();

    if (state->ui.tabWidget) {
        // Initial tab: System. After activation, default to Settings.
        state->ui.tabWidget->setCurrentIndex((state->cfg.activated && !state->cfg.licenseBlocked) ? 1 : 0);
        QObject::connect(state->ui.tabWidget, &QTabWidget::currentChanged, rawWindow, [state, refreshActivationUi, oldIdx = 0](int idx) mutable {
            // Prevent switching to Settings unless activated.
            if (idx == 1 && (!state->cfg.activated || state->cfg.licenseBlocked)) {
                if (state->ui.tabWidget) {
                    state->ui.tabWidget->blockSignals(true);
                    state->ui.tabWidget->setCurrentIndex(0);
                    state->ui.tabWidget->blockSignals(false);
                }
                state->highlightServerNameError = true;
                refreshActivationUi();
            }
            oldIdx = idx;
        });
    }

    if (state->ui.textEdit_0) {
        state->ui.textEdit_0->setMaxLength(20);
        state->ui.textEdit_0->setPlaceholderText("Enter Server Name");
        QObject::connect(state->ui.textEdit_0, &QLineEdit::textChanged, rawWindow, [state, sanitizeServerNameEditor, refreshActivationUi](const QString&) {
            sanitizeServerNameEditor();
            state->highlightServerNameError = false;
            refreshActivationUi();
        });
    }
    if (state->ui.textEdit_1) {
        if (state->ui.label_02) {
            state->ui.label_02->setText("Pairing Code");
        }
        state->ui.textEdit_1->setEchoMode(QLineEdit::Normal);
        state->ui.textEdit_1->setPlaceholderText("Paste pairing code from dashboard");
        QObject::connect(state->ui.textEdit_1, &QLineEdit::textChanged, rawWindow, [state, refreshActivationUi](const QString&) {
            refreshActivationUi();
        });
    }
    // textEdit_2/textEdit_3 hidden in v2.

    // Save ServerName button
    if (state->ui.pushButton_0) {
        QObject::connect(state->ui.pushButton_0, &QPushButton::clicked, rawWindow, [state, refreshActivationUi]() {
            if (!state->ui.textEdit_0) return;
            std::string newName = SanitizeServerName(Trim(state->ui.textEdit_0->text().toUtf8().toStdString()));
            if (state->ui.textEdit_0->text() != QString::fromUtf8(newName.c_str())) {
                state->ui.textEdit_0->setText(QString::fromUtf8(newName.c_str()));
            }
            if (!IsValidServerName(newName)) {
                state->highlightServerNameError = true;
                DebugLog("ControlPanel(System): ServerName is invalid; refusing to save.");
                refreshActivationUi();
                return;
            }
            (void)(newName != state->cfg.serverName);
            state->cfg.serverName = newName;
            state->cfg.machineId = GetMachineId();
            state->cfg.lanIp = GetLanIpv4();

            // If not activated yet, keep secrets empty.
            if (!state->cfg.activated) {
                ClearActivationSecrets(state->cfg);
            }

            state->highlightServerNameError = false;
            if (state->ui.textEdit_0) state->ui.textEdit_0->setModified(false);
            if (!SaveServerConfig(state->cfg)) {
                DebugLog("ControlPanel(System): Failed to save encrypted Server config.");
            }

            refreshActivationUi();
        });
    }

    // Activate button
    if (state->ui.pushButton_1) {
        QObject::connect(state->ui.pushButton_1, &QPushButton::clicked, rawWindow, [state, rawWindow, refreshActivationUi]() {
            if (!state->ui.textEdit_0 || !state->ui.textEdit_1) return;
            if (state->cfg.activated) {
                ShowActivationMessageBox(rawWindow, QMessageBox::Information,
                    "TaskTray Activation",
                    state->cfg.licenseBlocked
                        ? "This device is enrolled but the license is expired. Renew the license in the dashboard, then wait for automatic re-check."
                        : "This device is already enrolled.");
                refreshActivationUi();
                return;
            }

            const std::string uiName = SanitizeServerName(Trim(state->ui.textEdit_0->text().toUtf8().toStdString()));
            if (state->ui.textEdit_0->text() != QString::fromUtf8(uiName.c_str())) {
                state->ui.textEdit_0->setText(QString::fromUtf8(uiName.c_str()));
            }
            if (!IsValidServerName(uiName) || uiName != Trim(state->cfg.serverName)) {
                state->highlightServerNameError = true;
                DebugLog("ControlPanel(System): Refusing to activate because ServerName is not saved (press Save first).");
                refreshActivationUi(); return;
            }
            state->highlightServerNameError = false;
            const std::string pairingCode = Trim(state->ui.textEdit_1->text().toUtf8().toStdString());
            state->cfg.machineId = GetMachineId();
            state->cfg.lanIp = GetLanIpv4();
            state->cfg.entitlementExpiresAt.clear();
            state->cfg.lastSuccessRefreshAt.clear();

            if (state->cfg.serverName.empty() || pairingCode.empty()) {
                DebugLog("ControlPanel(System): Missing fields for enrollment.");
                ShowActivationMessageBox(rawWindow, QMessageBox::Warning,
                    "TaskTray Activation",
                    "Pairing code is required. Create it in the dashboard (Devices -> Add device) and paste it here.");
                return;
            }

            std::string pubKeyB64;
            std::string pubErr;
            if (!DeviceSignKey::GetOrCreatePublicKeyBase64(pubKeyB64, &pubErr) || pubKeyB64.empty()) {
                DebugLog(std::string("ControlPanel(System): GetOrCreatePublicKeyBase64 failed: ") + pubErr);
                ShowActivationMessageBox(rawWindow, QMessageBox::Warning,
                    "TaskTray Activation",
                    "Failed to generate device signing key. Run as administrator once, or check ProgramData permissions.");
                return;
            }

            std::wstring baseUrl = GetAppBaseUrlW();
            std::string body = std::string("{")
                + "\"pairing_code\":\"" + JsonEscape(pairingCode) + "\","
                + "\"role\":\"server\","
                + "\"machine_id\":\"" + JsonEscape(state->cfg.machineId) + "\","
                + "\"device_name\":\"" + JsonEscape(state->cfg.serverName) + "\","
                + "\"lan_ip\":\"" + JsonEscape(state->cfg.lanIp) + "\","
                + "\"device_pubkey_b64\":\"" + JsonEscape(pubKeyB64) + "\""
                + "}";

            const HttpJsonResult enr = WinHttpPostJsonEx(baseUrl, L"/api/device_enroll.php", body);
            if (!enr.transportOk || enr.body.empty()) {
                DebugLog("ControlPanel(System): device_enroll transport failed.");
                ShowActivationMessageBox(rawWindow, QMessageBox::Warning,
                    "TaskTray Activation",
                    "Could not contact the server. Check network connectivity and try again.");
                return;
            }

            if (enr.statusCode < 200 || enr.statusCode >= 300) {
                std::string err;
                std::string reason;
                (void)JsonExtractString(enr.body, "error", err);
                (void)JsonExtractString(enr.body, "reason", reason);
                if (reason == "license_expired") {
                    std::string exp;
                    if (JsonExtractString(enr.body, "entitlement_expires_at", exp)) {
                        state->cfg.entitlementExpiresAt = exp;
                    }
                    state->cfg.licenseBlocked = true;
                }
                ShowActivationMessageBox(rawWindow, QMessageBox::Warning,
                    "TaskTray Activation",
                    err.empty() ? "Enrollment failed." : QString::fromUtf8(err.c_str()));
                refreshActivationUi();
                return;
            }

            std::string deviceId;
            std::string refreshToken;
            std::string licenseBlob;
            std::string exp;

            if (!JsonExtractString(enr.body, "device_id", deviceId) || deviceId.empty() ||
                !JsonExtractString(enr.body, "refresh_token", refreshToken) || refreshToken.empty() ||
                !JsonExtractString(enr.body, "license_blob", licenseBlob) || licenseBlob.empty()) {
                DebugLog("ControlPanel(System): device_enroll parse failed.");
                ShowActivationMessageBox(rawWindow, QMessageBox::Warning,
                    "TaskTray Activation",
                    "Enrollment succeeded but the response was invalid. Please try again.");
                return;
            }
            (void)JsonExtractString(enr.body, "entitlement_expires_at", exp);

            state->cfg.deviceId = deviceId;
            state->cfg.refreshToken = refreshToken;
            state->cfg.licenseBlob = licenseBlob;
            state->cfg.entitlementExpiresAt = exp;
            state->cfg.lastSuccessRefreshAt = NowIsoLocal();
            state->cfg.licenseBlocked = false;
            state->cfg.activated = true;

            if (state->ui.textEdit_1) {
                state->ui.textEdit_1->setText("");
                state->ui.textEdit_1->setModified(false);
            }
            if (!SaveServerConfig(state->cfg)) {
                DebugLog("ControlPanel(System): Failed to save encrypted Server config after enrollment.");
            }

            ShowActivationMessageBox(rawWindow, QMessageBox::Information,
                "TaskTray Activation",
                "Enrollment completed. You can now open Settings.");
            refreshActivationUi();
        });
    }

    // NOTE: Activation validity polling is handled by TaskTrayApp's background thread.
    auto* activationUiTimer = new QTimer(rawWindow);
    activationUiTimer->setInterval(2000);
    QObject::connect(activationUiTimer, &QTimer::timeout, rawWindow, [state, refreshActivationUi, syncActivationEditorsFromConfig]() {
        ServerActivationConfig latest;
        latest.machineId = GetMachineId();
        latest.lanIp = GetLanIpv4();
        if (LoadServerConfig(latest)) {
            const bool activatedChanged = (latest.activated != state->cfg.activated);
            const bool expiryChanged = (latest.entitlementExpiresAt != state->cfg.entitlementExpiresAt);
            const bool licenseBlockChanged = (latest.licenseBlocked != state->cfg.licenseBlocked);
            const bool lastCheckChanged = (latest.lastSuccessRefreshAt != state->cfg.lastSuccessRefreshAt);
            const bool savedNameChanged = (latest.serverName != state->cfg.serverName);
            if (activatedChanged || expiryChanged || licenseBlockChanged || lastCheckChanged || savedNameChanged) {
                state->cfg = latest;
                syncActivationEditorsFromConfig(true);
                refreshActivationUi();
            }
        } else if (!state->cfg.activated) {
            refreshActivationUi();
        }
    });
    activationUiTimer->start();

    // Checkbox exclusive: only one can be selected at a time
    QCheckBox* cb1 = state->ui.checkBox_1;
    QCheckBox* cb2 = state->ui.checkBox_2;
    QCheckBox* cb3 = state->ui.checkBox_3;
    QPushButton* saveButton = state->ui.pushButton_2;

    if (TaskTrayApp* appInst = g_taskTrayAppInstance.load()) {
        int plan = appInst->GetOptimizedPlanForSync();
        if (cb1 && cb2 && cb3) {
            cb1->setChecked(plan == 1);
            cb2->setChecked(plan == 2);
            cb3->setChecked(plan == 3);
        }
    }

    QObject::connect(cb1, &QCheckBox::toggled, rawWindow, [cb2, cb3](bool checked) {
        if (checked) {
            if (cb2) cb2->setChecked(false);
            if (cb3) cb3->setChecked(false);
        }
    });

    QObject::connect(cb2, &QCheckBox::toggled, rawWindow, [cb1, cb3](bool checked) {
        if (checked) {
            if (cb1) cb1->setChecked(false);
            if (cb3) cb3->setChecked(false);
        }
    });

    QObject::connect(cb3, &QCheckBox::toggled, rawWindow, [cb1, cb2](bool checked) {
        if (checked) {
            if (cb1) cb1->setChecked(false);
            if (cb2) cb2->setChecked(false);
        }
    });

    if (saveButton) {
        QObject::connect(saveButton, &QPushButton::clicked, rawWindow, [cb1, cb2, cb3]() {
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

    rawWindow->setAttribute(Qt::WA_DeleteOnClose, false);

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

    DebugLog("ShowControlPanel: Control panel window initialized.");

    g_controlPanelWindow.store(rawWindow);

    auto closeFilter = new ControlPanelCloseFilter(rawWindow);
    rawWindow->installEventFilter(closeFilter);

    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, rawWindow, [rawWindow, resetState]() {
        DebugLog("ShowControlPanel: QApplication about to quit.");
        resetState();
        rawWindow->hide();
        rawWindow->deleteLater();
    });

    QObject::connect(rawWindow, &QObject::destroyed, [resetState]() {
        DebugLog("ShowControlPanel: Control panel window destroyed.");
        resetState();
    });

    mainWindow.release();
    rawWindow->show();
    rawWindow->raise();
    rawWindow->activateWindow();

    // トレイメニュー経由で開いた直後はフォーカスがメニュー側に残ることがあり、
    // QLineEdit へ入力できないことがある。
    // そのため、表示直後にネイティブ側でも前面化・フォーカスを強制する。
    auto activateAndFocus = [rawWindow]() {
        if (!rawWindow) {
            return;
        }

        rawWindow->showNormal();
        rawWindow->raise();
        rawWindow->activateWindow();

        if (HWND windowHandle = reinterpret_cast<HWND>(rawWindow->winId())) {
            ForceForegroundWindow(windowHandle);
        }

        if (QWidget* focusTarget = rawWindow->findChild<QLineEdit*>("textEdit_0")) {
            focusTarget->setFocus(Qt::ActiveWindowFocusReason);
        }
        else if (QWidget* focusTarget = rawWindow->focusWidget()) {
            focusTarget->setFocus(Qt::ActiveWindowFocusReason);
        }
    };

    QTimer::singleShot(0, rawWindow, activateAndFocus);
    QTimer::singleShot(50, rawWindow, activateAndFocus);
}

LRESULT CALLBACK TaskTrayApp::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    TaskTrayApp* app = nullptr;
    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        app = reinterpret_cast<TaskTrayApp*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        EnsureTrayHostWindowHidden(hwnd);
    }
    else {
        app = reinterpret_cast<TaskTrayApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (app) {
        switch (uMsg) {
        case WM_SHOWWINDOW:
            if (wParam) {
                DebugLog("WindowProc: Suppressing unexpected tray host window show.");
                EnsureTrayHostWindowHidden(hwnd);
                return 0;
            }
            break;

        case WM_WINDOWPOSCHANGING:
        {
            WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(lParam);
            if (wp) {
                // Keep the tray host window hidden and off-screen, but allow activation when needed
                // (e.g., SetForegroundWindow + TrackPopupMenu pattern).
                wp->flags |= SWP_NOOWNERZORDER | SWP_NOSIZE | SWP_NOMOVE;
            }
            break;
        }

        case WM_ACTIVATEAPP:
        case WM_NCACTIVATE:
        case WM_MOUSEACTIVATE:
            EnsureTrayHostWindowHidden(hwnd);
            break;
        }

        if (uMsg == WM_TASKBARCREATED) {
            DebugLog("WindowProc: TaskbarCreated message received. Re-creating icon.");
            EnsureTrayHostWindowHidden(hwnd);
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
            // Display configuration changed. Post a message to ourselves to update the UI.
            // The background thread will handle the logic, this just updates the tooltip.
            PostMessage(app->hwnd, WM_USER + 2, 0, 0);
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_EXIT) { // Exit command
                DebugLog("WindowProc: Exit command received.");
                if (app->Cleanup()) {
                    DebugLog("WindowProc: Cleanup succeeded.");
                    if (!QCoreApplication::instance()) {
                        PostQuitMessage(0);
                    }
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
            if (QCoreApplication::instance()) {
                QCoreApplication::quit();
            } else {
                PostQuitMessage(0);
            }
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
    if (!EnsureQtApplicationCreated()) {
        DebugLog("TaskTrayApp::Run: Failed to create QApplication.");
        return -1;
    }

    DebugLog("TaskTrayApp::Run: Entering QApplication event loop.");
    return g_qtApplication->exec();
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
