#define _WIN32_WINNT 0x0601
#define WINVER 0x0601

#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <rpc.h>
#include <rpcndr.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "TrayService.h"
}

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {
constexpr wchar_t kServiceName[] = L"TrayAppService";
constexpr wchar_t kRpcEndpoint[] = L"TrayServiceEndpoint";
constexpr wchar_t kAppName[] = L"TrayApp.exe";
constexpr wchar_t kDefaultServerUrl[] = L"https://10.211.55.1:8443";
constexpr long kErrorNotAuthenticated = 1001;
constexpr long kErrorNoLicense = 2001;
constexpr long kErrorNetwork = 3001;
constexpr long kErrorBadResponse = 3002;

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_stateChangedEvent = nullptr;
HANDLE g_backgroundThread = nullptr;
CRITICAL_SECTION g_processesLock;
CRITICAL_SECTION g_stateLock;
std::map<DWORD, HANDLE> g_sessionProcesses;

struct HttpResponse {
    DWORD status = 0;
    std::string body;
};

struct ServiceState {
    std::wstring serverUrl = kDefaultServerUrl;
    long productId = 1;
    std::wstring email;
    std::string accessToken;
    std::string refreshToken;
    ULONGLONG accessExpiry = 0;
    ULONGLONG refreshExpiry = 0;
    std::string licenseTicketJson;
    std::string licenseSignature;
    std::wstring activationDate;
    std::wstring expirationDate;
    std::wstring deviceMac;
    bool licenseBlocked = false;
    ULONGLONG licenseRefreshAt = 0;
    std::wstring lastAuthError;
    std::wstring lastLicenseError;
};

ServiceState g_state;

ULONGLONG NowMs() {
    return GetTickCount64();
}

void CopyString(wchar_t* dest, size_t count, const std::wstring& value) {
    if (!dest || count == 0) return;
    wcsncpy_s(dest, count, value.c_str(), _TRUNCATE);
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(needed > 0 ? needed : 0), '\0');
    if (needed > 1) {
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), needed, nullptr, nullptr);
        result.pop_back();
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(needed > 0 ? needed : 0), L'\0');
    if (needed > 1) {
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
        result.pop_back();
    }
    return result;
}

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    for (char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += ch; break;
        }
    }
    return escaped;
}

std::string JsonString(const std::string& json, const char* key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    std::string result;
    bool escape = false;
    for (++pos; pos < json.size(); ++pos) {
        char ch = json[pos];
        if (escape) {
            result += ch;
            escape = false;
        } else if (ch == '\\') {
            escape = true;
        } else if (ch == '"') {
            break;
        } else {
            result += ch;
        }
    }
    return result;
}

std::string JsonValueAsString(const std::string& json, const char* key) {
    std::string stringValue = JsonString(json, key);
    if (!stringValue.empty()) return stringValue;
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return {};
    while (++pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) {}
    const size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && !isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    return json.substr(start, pos - start);
}

long long JsonInt64(const std::string& json, const char* key, long long fallback = 0) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return fallback;
    while (++pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) {}
    try {
        return std::stoll(json.substr(pos));
    } catch (...) {
        return fallback;
    }
}

bool JsonBool(const std::string& json, const char* key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return false;
    while (++pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) {}
    return json.compare(pos, 4, "true") == 0;
}

std::string JsonObject(const std::string& json, const char* key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return {};
    pos = json.find('{', pos + pattern.size());
    if (pos == std::string::npos) return {};
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t i = pos; i < json.size(); ++i) {
        const char ch = json[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') inString = !inString;
        if (inString) continue;
        if (ch == '{') ++depth;
        if (ch == '}' && --depth == 0) return json.substr(pos, i - pos + 1);
    }
    return {};
}

ULONGLONG ParseServerExpiryMs(const std::string& value, ULONGLONG fallbackDeltaMs) {
    if (value.empty()) return NowMs() + fallbackDeltaMs;
    if (std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const unsigned long long epochMs = _strtoui64(value.c_str(), nullptr, 10);
        const unsigned long long nowEpochMs = static_cast<unsigned long long>(time(nullptr)) * 1000ULL;
        return NowMs() + (epochMs > nowEpochMs ? epochMs - nowEpochMs : 0);
    }

    std::tm tm = {};
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (sscanf_s(value.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) == 6) {
        tm.tm_year = year - 1900;
        tm.tm_mon = mon - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        const time_t epoch = _mkgmtime(&tm);
        const time_t now = time(nullptr);
        return NowMs() + (epoch > now ? static_cast<ULONGLONG>(epoch - now) * 1000ULL : 0);
    }
    return NowMs() + fallbackDeltaMs;
}

std::wstring ReadEnvString(const wchar_t* name, const wchar_t* fallback) {
    wchar_t buffer[1024] = {};
    DWORD count = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return count > 0 && count < std::size(buffer) ? std::wstring(buffer) : std::wstring(fallback);
}

long ReadEnvLong(const wchar_t* name, long fallback) {
    wchar_t buffer[64] = {};
    DWORD count = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return count > 0 ? wcstol(buffer, nullptr, 10) : fallback;
}

bool ReadEnvBool(const wchar_t* name, bool fallback) {
    wchar_t buffer[64] = {};
    DWORD count = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (count == 0 || count >= std::size(buffer)) return fallback;
    return wcscmp(buffer, L"0") != 0 && _wcsicmp(buffer, L"false") != 0 && _wcsicmp(buffer, L"no") != 0;
}

std::wstring GetDeviceName() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    return GetComputerNameW(buffer, &size) ? std::wstring(buffer) : L"Windows device";
}

std::wstring GetDeviceMac() {
    ULONG size = 15000;
    std::vector<BYTE> bytes(size);
    IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        bytes.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, adapters, &size);
    }
    if (result == NO_ERROR) {
        for (auto* a = adapters; a != nullptr; a = a->Next) {
            if (a->PhysicalAddressLength >= 6 && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
                wchar_t mac[32] = {};
                swprintf_s(mac, std::size(mac), L"%02X:%02X:%02X:%02X:%02X:%02X",
                           a->PhysicalAddress[0], a->PhysicalAddress[1], a->PhysicalAddress[2],
                           a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
                return mac;
            }
        }
    }
    return L"00:00:00:00:00:00";
}

HttpResponse HttpPost(const std::wstring& baseUrl, const wchar_t* path, const std::string& body, const std::string& bearerToken = {}) {
    HttpResponse response;
    URL_COMPONENTSW parts = {};
    wchar_t host[256] = {};
    wchar_t urlPath[1024] = {};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = urlPath;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(urlPath));
    if (!WinHttpCrackUrl(baseUrl.c_str(), 0, 0, &parts)) return response;

    std::wstring hostName(host, parts.dwHostNameLength);
    std::wstring fullPath = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (!fullPath.empty() && fullPath.back() == L'/') fullPath.pop_back();
    fullPath += path;

    HINTERNET session = WinHttpOpen(L"TrayAppService/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return response;
    HINTERNET connect = WinHttpConnect(session, hostName.c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return response;
    }
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", fullPath.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return response;
    }

    if (parts.nScheme == INTERNET_SCHEME_HTTPS && ReadEnvBool(L"TRAYAPP_ALLOW_INSECURE_TLS", true)) {
        DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                              SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }

    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!bearerToken.empty()) headers += L"Authorization: Bearer " + Utf8ToWide(bearerToken) + L"\r\n";

    BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                 const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                 static_cast<DWORD>(body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD statusSize = sizeof(response.status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            chunk.resize(read);
            response.body += chunk;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

void ClearLicenseLocked(const std::wstring& reason = L"Лицензия отсутствует") {
    g_state.licenseTicketJson.clear();
    g_state.licenseSignature.clear();
    g_state.activationDate.clear();
    g_state.expirationDate.clear();
    g_state.deviceMac.clear();
    g_state.licenseBlocked = false;
    g_state.licenseRefreshAt = 0;
    g_state.lastLicenseError = reason;
}

void ClearAuthLocked() {
    g_state.email.clear();
    g_state.accessToken.clear();
    g_state.refreshToken.clear();
    g_state.accessExpiry = 0;
    g_state.refreshExpiry = 0;
    g_state.lastAuthError.clear();
    ClearLicenseLocked();
}

bool StoreTokenResponseLocked(const std::wstring& email, const std::string& body) {
    const std::string accessToken = JsonString(body, "accessToken");
    const std::string refreshToken = JsonString(body, "refreshToken");
    if (accessToken.empty() || refreshToken.empty()) return false;
    g_state.email = email;
    g_state.accessToken = accessToken;
    g_state.refreshToken = refreshToken;
    g_state.accessExpiry = ParseServerExpiryMs(JsonValueAsString(body, "accessTokenExpiry"), 5 * 60 * 1000ULL);
    g_state.refreshExpiry = ParseServerExpiryMs(JsonValueAsString(body, "refreshTokenExpiry"), 30 * 60 * 1000ULL);
    g_state.lastAuthError.clear();
    return true;
}

bool StoreTicketResponseLocked(const std::string& body) {
    const std::string ticket = JsonObject(body, "ticket");
    if (ticket.empty()) return false;
    const long long ttl = std::max<long long>(JsonInt64(ticket, "ttlMillis", 60000), 5000);
    g_state.licenseTicketJson = ticket;
    g_state.licenseSignature = JsonString(body, "signature");
    g_state.activationDate = Utf8ToWide(JsonString(ticket, "activationDate"));
    g_state.expirationDate = Utf8ToWide(JsonString(ticket, "expirationDate"));
    g_state.deviceMac = Utf8ToWide(JsonString(ticket, "deviceMac"));
    g_state.licenseBlocked = JsonBool(ticket, "blocked");
    g_state.licenseRefreshAt = NowMs() + static_cast<ULONGLONG>(ttl * 8 / 10);
    g_state.lastLicenseError.clear();
    return true;
}

bool RefreshTokens() {
    std::wstring serverUrl;
    std::wstring email;
    std::string refreshToken;
    EnterCriticalSection(&g_stateLock);
    serverUrl = g_state.serverUrl;
    email = g_state.email;
    refreshToken = g_state.refreshToken;
    LeaveCriticalSection(&g_stateLock);
    if (refreshToken.empty()) return false;

    const std::string body = "{\"refreshToken\":\"" + JsonEscape(refreshToken) + "\"}";
    const HttpResponse response = HttpPost(serverUrl, L"/auth/refresh", body);
    EnterCriticalSection(&g_stateLock);
    const bool ok = response.status == 200 && StoreTokenResponseLocked(email, response.body);
    if (!ok) {
        ClearAuthLocked();
        g_state.lastAuthError = L"Сессия истекла. Войдите заново.";
    }
    LeaveCriticalSection(&g_stateLock);
    SetEvent(g_stateChangedEvent);
    return ok;
}

bool CheckLicense() {
    std::wstring serverUrl;
    std::string accessToken;
    long productId = 1;
    std::wstring mac = GetDeviceMac();
    EnterCriticalSection(&g_stateLock);
    serverUrl = g_state.serverUrl;
    accessToken = g_state.accessToken;
    productId = g_state.productId;
    LeaveCriticalSection(&g_stateLock);
    if (accessToken.empty()) return false;

    std::ostringstream body;
    body << "{\"deviceMac\":\"" << JsonEscape(WideToUtf8(mac)) << "\",\"productId\":" << productId << "}";
    const HttpResponse response = HttpPost(serverUrl, L"/api/licenses/check", body.str(), accessToken);
    EnterCriticalSection(&g_stateLock);
    const bool ok = response.status == 200 && StoreTicketResponseLocked(response.body);
    if (!ok) ClearLicenseLocked(L"Активная лицензия не найдена");
    LeaveCriticalSection(&g_stateLock);
    SetEvent(g_stateChangedEvent);
    return ok;
}

DWORD CalculateBackgroundWaitMs() {
    EnterCriticalSection(&g_stateLock);
    DWORD waitMs = 60000;
    if (!g_state.refreshToken.empty() && g_state.accessExpiry > 0) {
        ULONGLONG tokenExpiry = g_state.accessExpiry;
        if (g_state.refreshExpiry > 0) tokenExpiry = std::min(tokenExpiry, g_state.refreshExpiry);
        const ULONGLONG target = tokenExpiry > 60000 ? tokenExpiry - 60000 : tokenExpiry;
        waitMs = static_cast<DWORD>(target > NowMs() ? std::min<ULONGLONG>(target - NowMs(), waitMs) : 0);
    }
    if (!g_state.licenseTicketJson.empty() && g_state.licenseRefreshAt > 0) {
        waitMs = static_cast<DWORD>(g_state.licenseRefreshAt > NowMs() ? std::min<ULONGLONG>(g_state.licenseRefreshAt - NowMs(), waitMs) : 0);
    }
    if (g_state.refreshToken.empty() && g_state.licenseTicketJson.empty()) waitMs = INFINITE;
    LeaveCriticalSection(&g_stateLock);
    return waitMs;
}

DWORD WINAPI BackgroundWorkerThread(LPVOID) {
    HANDLE events[] = { g_stopEvent, g_stateChangedEvent };
    for (;;) {
        DWORD result = WaitForMultipleObjects(2, events, FALSE, CalculateBackgroundWaitMs());
        if (result == WAIT_OBJECT_0) return 0;
        if (result == WAIT_OBJECT_0 + 1) continue;

        bool needTokenRefresh = false;
        bool needLicenseRefresh = false;
        EnterCriticalSection(&g_stateLock);
        needTokenRefresh = !g_state.refreshToken.empty() &&
                           ((g_state.accessExpiry > 0 && g_state.accessExpiry <= NowMs() + 60000) ||
                            (g_state.refreshExpiry > 0 && g_state.refreshExpiry <= NowMs() + 60000));
        needLicenseRefresh = !g_state.licenseTicketJson.empty() && g_state.licenseRefreshAt <= NowMs();
        LeaveCriticalSection(&g_stateLock);

        if (needTokenRefresh) RefreshTokens();
        if (needLicenseRefresh) CheckLicense();
    }
}

std::wstring GetTrayAppPath() {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring serviceDir = modulePath;
    auto pos = serviceDir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) serviceDir.erase(pos);

    std::wstring candidates[] = { serviceDir + L"\\" + kAppName, serviceDir + L"\\..\\" + kAppName, serviceDir + L"\\..\\Release\\" + kAppName };
    for (const auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return candidates[0];
}

void UpdateServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR) {
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = win32ExitCode;
    g_serviceStatus.dwWaitHint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : 5000;
    g_serviceStatus.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_SESSIONCHANGE : 0;
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_serviceStatus);
}

void LaunchAppInSession(DWORD sessionId) {
    if (sessionId == 0) return;
    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) return;

    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(userToken, TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                          nullptr, SecurityImpersonation, TokenPrimary, &primaryToken)) {
        CloseHandle(userToken);
        return;
    }
    CloseHandle(userToken);

    LPVOID env = nullptr;
    CreateEnvironmentBlock(&env, primaryToken, FALSE);
    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::wstring appPath = GetTrayAppPath();
    std::wstring cmdLine = L"\"" + appPath + L"\" /hidden";
    BOOL ok = CreateProcessAsUserW(primaryToken, appPath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT, env, nullptr, &si, &pi);

    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(primaryToken);
    if (ok) {
        EnterCriticalSection(&g_processesLock);
        if (g_sessionProcesses.count(sessionId)) {
            TerminateProcess(g_sessionProcesses[sessionId], 0);
            CloseHandle(g_sessionProcesses[sessionId]);
        }
        g_sessionProcesses[sessionId] = pi.hProcess;
        LeaveCriticalSection(&g_processesLock);
        CloseHandle(pi.hThread);
    } else if (pi.hProcess) {
        CloseHandle(pi.hProcess);
    }
}

void LaunchAppInExistingSessions() {
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        for (DWORD i = 0; i < count; ++i) {
            if (sessions[i].SessionId != 0) LaunchAppInSession(sessions[i].SessionId);
        }
        WTSFreeMemory(sessions);
    }
}

void TerminateAllApps() {
    EnterCriticalSection(&g_processesLock);
    for (auto& p : g_sessionProcesses) {
        if (p.second) {
            TerminateProcess(p.second, 0);
            WaitForSingleObject(p.second, 5000);
            CloseHandle(p.second);
        }
    }
    g_sessionProcesses.clear();
    LeaveCriticalSection(&g_processesLock);
}

DWORD WINAPI ServiceWorkerThread(LPVOID) {
    g_state.serverUrl = ReadEnvString(L"TRAYAPP_SERVER_URL", kDefaultServerUrl);
    g_state.productId = ReadEnvLong(L"TRAYAPP_PRODUCT_ID", 1);

    RPC_STATUS status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)kRpcEndpoint, nullptr);
    if (status == RPC_S_OK) status = RpcServerRegisterIf(ITrayService_v1_0_s_ifspec, nullptr, nullptr);
    if (status == RPC_S_OK) status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    if (status != RPC_S_OK) {
        UpdateServiceState(SERVICE_STOPPED, status);
        return status;
    }

    g_backgroundThread = CreateThread(nullptr, 0, BackgroundWorkerThread, nullptr, 0, nullptr);
    UpdateServiceState(SERVICE_RUNNING);
    LaunchAppInExistingSessions();
    WaitForSingleObject(g_stopEvent, INFINITE);

    UpdateServiceState(SERVICE_STOP_PENDING);
    RpcMgmtStopServerListening(nullptr);
    RpcServerUnregisterIf(ITrayService_v1_0_s_ifspec, nullptr, FALSE);
    if (g_backgroundThread) {
        SetEvent(g_stateChangedEvent);
        WaitForSingleObject(g_backgroundThread, 5000);
        CloseHandle(g_backgroundThread);
        g_backgroundThread = nullptr;
    }
    TerminateAllApps();
    return 0;
}
}  // namespace

extern "C" {
error_status_t StopService() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    return RPC_S_OK;
}

error_status_t GetServiceStatus(long* status) {
    if (status) *status = g_serviceStatus.dwCurrentState;
    return RPC_S_OK;
}

error_status_t GetCurrentUser(AuthUserInfo* info) {
    if (!info) return RPC_X_NULL_REF_POINTER;
    ZeroMemory(info, sizeof(*info));
    EnterCriticalSection(&g_stateLock);
    info->authenticated = !g_state.refreshToken.empty() ? 1 : 0;
    info->errorCode = info->authenticated ? 0 : kErrorNotAuthenticated;
    CopyString(info->email, std::size(info->email), g_state.email);
    CopyString(info->message, std::size(info->message), info->authenticated ? L"Пользователь аутентифицирован" : g_state.lastAuthError);
    LeaveCriticalSection(&g_stateLock);
    return RPC_S_OK;
}

error_status_t Login(wchar_t* email, wchar_t* password, AuthUserInfo* info) {
    if (!email || !password || !info) return RPC_X_NULL_REF_POINTER;
    ZeroMemory(info, sizeof(*info));
    std::wstring serverUrl;
    EnterCriticalSection(&g_stateLock);
    serverUrl = g_state.serverUrl;
    LeaveCriticalSection(&g_stateLock);

    const std::string body = "{\"email\":\"" + JsonEscape(WideToUtf8(email)) + "\",\"password\":\"" + JsonEscape(WideToUtf8(password)) + "\"}";
    const HttpResponse response = HttpPost(serverUrl, L"/auth/login", body);

    EnterCriticalSection(&g_stateLock);
    if (response.status == 200 && StoreTokenResponseLocked(email, response.body)) {
        ClearLicenseLocked();
        info->authenticated = 1;
        info->errorCode = 0;
        CopyString(info->email, std::size(info->email), g_state.email);
        CopyString(info->message, std::size(info->message), L"Вход выполнен");
    } else {
        ClearAuthLocked();
        g_state.lastAuthError = response.status == 0 ? L"Сервер недоступен" : L"Неверный email или пароль";
        info->authenticated = 0;
        info->errorCode = response.status == 0 ? kErrorNetwork : kErrorBadResponse;
        CopyString(info->message, std::size(info->message), g_state.lastAuthError);
    }
    LeaveCriticalSection(&g_stateLock);
    SetEvent(g_stateChangedEvent);
    return RPC_S_OK;
}

error_status_t Logout() {
    std::wstring serverUrl;
    std::string refreshToken;
    EnterCriticalSection(&g_stateLock);
    serverUrl = g_state.serverUrl;
    refreshToken = g_state.refreshToken;
    LeaveCriticalSection(&g_stateLock);
    if (!refreshToken.empty()) {
        const std::string body = "{\"refreshToken\":\"" + JsonEscape(refreshToken) + "\"}";
        HttpPost(serverUrl, L"/auth/logout", body);
    }
    EnterCriticalSection(&g_stateLock);
    ClearAuthLocked();
    LeaveCriticalSection(&g_stateLock);
    SetEvent(g_stateChangedEvent);
    return RPC_S_OK;
}

error_status_t GetLicenseInfo(LicenseInfo* info) {
    if (!info) return RPC_X_NULL_REF_POINTER;
    ZeroMemory(info, sizeof(*info));
    bool shouldCheck = false;
    EnterCriticalSection(&g_stateLock);
    if (g_state.refreshToken.empty()) {
        info->errorCode = kErrorNotAuthenticated;
        CopyString(info->message, std::size(info->message), L"Пользователь не аутентифицирован");
    } else if (g_state.licenseTicketJson.empty()) {
        shouldCheck = true;
    }
    LeaveCriticalSection(&g_stateLock);

    if (shouldCheck) CheckLicense();

    EnterCriticalSection(&g_stateLock);
    info->hasLicense = !g_state.licenseTicketJson.empty() ? 1 : 0;
    info->blocked = g_state.licenseBlocked ? 1 : 0;
    info->errorCode = info->hasLicense && !info->blocked ? 0 : kErrorNoLicense;
    CopyString(info->activationDate, std::size(info->activationDate), g_state.activationDate);
    CopyString(info->expirationDate, std::size(info->expirationDate), g_state.expirationDate);
    CopyString(info->deviceMac, std::size(info->deviceMac), g_state.deviceMac);
    CopyString(info->message, std::size(info->message),
               info->hasLicense ? (info->blocked ? L"Лицензия заблокирована" : L"Лицензия активна") : g_state.lastLicenseError);
    LeaveCriticalSection(&g_stateLock);
    return RPC_S_OK;
}

error_status_t ActivateProduct(wchar_t* activationKey, LicenseInfo* info) {
    if (!activationKey || !info) return RPC_X_NULL_REF_POINTER;
    ZeroMemory(info, sizeof(*info));
    std::wstring serverUrl;
    std::string accessToken;
    EnterCriticalSection(&g_stateLock);
    serverUrl = g_state.serverUrl;
    accessToken = g_state.accessToken;
    LeaveCriticalSection(&g_stateLock);
    if (accessToken.empty()) {
        info->errorCode = kErrorNotAuthenticated;
        CopyString(info->message, std::size(info->message), L"Пользователь не аутентифицирован");
        return RPC_S_OK;
    }

    const std::wstring mac = GetDeviceMac();
    const std::wstring deviceName = GetDeviceName();
    const std::string body = "{\"activationKey\":\"" + JsonEscape(WideToUtf8(activationKey)) +
                             "\",\"deviceMac\":\"" + JsonEscape(WideToUtf8(mac)) +
                             "\",\"deviceName\":\"" + JsonEscape(WideToUtf8(deviceName)) + "\"}";
    const HttpResponse response = HttpPost(serverUrl, L"/api/licenses/activate", body, accessToken);
    EnterCriticalSection(&g_stateLock);
    bool ok = response.status == 200 && StoreTicketResponseLocked(response.body);
    if (!ok) {
        ClearLicenseLocked(response.status == 0 ? L"Сервер недоступен" : L"Не удалось активировать продукт");
    }
    LeaveCriticalSection(&g_stateLock);
    if (response.status == 200 && !ok) CheckLicense();
    SetEvent(g_stateChangedEvent);
    return GetLicenseInfo(info);
}
}

void* __RPC_USER MIDL_user_allocate(size_t s) { return malloc(s); }
void __RPC_USER MIDL_user_free(void* p) { free(p); }

DWORD WINAPI ServiceControlHandlerEx(DWORD ctrl, DWORD evtType, LPVOID evtData, LPVOID) {
    if (ctrl == SERVICE_CONTROL_SESSIONCHANGE && (evtType == WTS_SESSION_LOGON || evtType == WTS_CONSOLE_CONNECT || evtType == WTS_REMOTE_CONNECT)) {
        auto* notif = static_cast<WTSSESSION_NOTIFICATION*>(evtData);
        if (notif && notif->dwSessionId != 0) LaunchAppInSession(notif->dwSessionId);
    }
    return NO_ERROR;
}

VOID WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceControlHandlerEx, nullptr);
    if (!g_statusHandle) return;
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    UpdateServiceState(SERVICE_START_PENDING);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stateChangedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_stateChangedEvent) {
        UpdateServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    HANDLE worker = CreateThread(nullptr, 0, ServiceWorkerThread, nullptr, 0, nullptr);
    if (worker) {
        WaitForSingleObject(worker, INFINITE);
        DWORD workerExitCode = NO_ERROR;
        GetExitCodeThread(worker, &workerExitCode);
        CloseHandle(worker);
        CloseHandle(g_stopEvent);
        CloseHandle(g_stateChangedEvent);
        UpdateServiceState(SERVICE_STOPPED, workerExitCode);
        return;
    }

    DWORD error = GetLastError();
    CloseHandle(g_stopEvent);
    CloseHandle(g_stateChangedEvent);
    UpdateServiceState(SERVICE_STOPPED, error);
}

int wmain() {
    InitializeCriticalSection(&g_processesLock);
    InitializeCriticalSection(&g_stateLock);
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(kServiceName), ServiceMain }, { nullptr, nullptr } };
    StartServiceCtrlDispatcherW(table);
    DeleteCriticalSection(&g_stateLock);
    DeleteCriticalSection(&g_processesLock);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) { return wmain(); }
