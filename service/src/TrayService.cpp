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
#include <cstring>
#include <cwctype>
#include <ctime>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
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
constexpr wchar_t kDefaultServerUrls[] = L"https://10.211.55.2:8443;https://10.211.55.1:8443;https://localhost:8443";
constexpr long kErrorNotAuthenticated = 1001;
constexpr long kErrorNoLicense = 2001;
constexpr long kErrorNetwork = 3001;
constexpr long kErrorBadResponse = 3002;
constexpr long kErrorAvDatabaseNotLoaded = 4001;
constexpr long kErrorScanFailed = 4002;
constexpr size_t kHashSize = 32;

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_stateChangedEvent = nullptr;
HANDLE g_backgroundThread = nullptr;
HANDLE g_monitorThread = nullptr;
CRITICAL_SECTION g_processesLock;
CRITICAL_SECTION g_stateLock;
CRITICAL_SECTION g_avLock;
std::map<DWORD, HANDLE> g_sessionProcesses;

struct HttpResponse {
    DWORD status = 0;
    std::string body;
};

struct ServiceState {
    std::wstring serverUrl = L"https://10.211.55.2:8443";
    std::vector<std::wstring> serverUrls;
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

enum class ObjectType : unsigned char {
    PeFile = 1,
    Script = 2,
};

struct AvRecord {
    unsigned long long objectSignaturePrefix = 0;
    unsigned int objectSignatureLength = 0;
    std::vector<unsigned char> objectSignature;
    unsigned long long offsetBegin = 0;
    unsigned long long offsetEnd = 0;
    ObjectType objectType = ObjectType::PeFile;
    std::vector<unsigned char> avRecordSignature;
    std::wstring threatName;
};

struct AvDatabase {
    std::map<unsigned long long, std::vector<AvRecord>> recordsByPrefix;
    std::wstring releaseDate;
    bool loaded = false;
};

struct AhoNode {
    std::map<unsigned char, int> next;
    int link = 0;
    std::vector<unsigned long long> prefixes;
};

AvDatabase g_avDatabase;
std::vector<AhoNode> g_ahoTrie;
bool g_scheduleEnabled = false;
DWORD g_scheduleIntervalMinutes = 60;
std::wstring g_schedulePath;
ULONGLONG g_nextScheduledScan = 0;
bool g_monitorEnabled = false;
std::wstring g_monitorPath;

ULONGLONG NowMs() {
    return GetTickCount64();
}

void CopyString(wchar_t* dest, size_t count, const std::wstring& value) {
    if (!dest || count == 0) return;
    wcsncpy_s(dest, count, value.c_str(), _TRUNCATE);
}

unsigned long long ReadLe64(const unsigned char* bytes) {
    unsigned long long value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

std::vector<unsigned char> Le64Bytes(unsigned long long value) {
    std::vector<unsigned char> bytes(8);
    for (int i = 0; i < 8; ++i) bytes[static_cast<size_t>(i)] = static_cast<unsigned char>((value >> (i * 8)) & 0xff);
    return bytes;
}

std::vector<unsigned char> PseudoSha256(const std::vector<unsigned char>& data) {
    const unsigned long long seeds[4] = {
        1469598103934665603ULL, 1099511628211ULL, 7809847782465536322ULL, 9650029242287828579ULL
    };
    std::vector<unsigned char> digest(kHashSize);
    for (int lane = 0; lane < 4; ++lane) {
        unsigned long long hash = seeds[lane];
        for (unsigned char byte : data) {
            hash ^= static_cast<unsigned long long>(byte) + static_cast<unsigned long long>(lane * 17);
            hash *= 1099511628211ULL;
            hash ^= hash >> 32;
        }
        for (int i = 0; i < 8; ++i) {
            digest[static_cast<size_t>(lane * 8 + i)] = static_cast<unsigned char>((hash >> (i * 8)) & 0xff);
        }
    }
    return digest;
}

std::vector<unsigned char> RecordBytesForSignature(const AvRecord& record) {
    std::vector<unsigned char> bytes = Le64Bytes(record.objectSignaturePrefix);
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>((record.objectSignatureLength >> (i * 8)) & 0xff));
    bytes.insert(bytes.end(), record.objectSignature.begin(), record.objectSignature.end());
    std::vector<unsigned char> offsetBegin = Le64Bytes(record.offsetBegin);
    std::vector<unsigned char> offsetEnd = Le64Bytes(record.offsetEnd);
    bytes.insert(bytes.end(), offsetBegin.begin(), offsetBegin.end());
    bytes.insert(bytes.end(), offsetEnd.begin(), offsetEnd.end());
    bytes.push_back(static_cast<unsigned char>(record.objectType));
    return bytes;
}

bool VerifyRecordSignature(const AvRecord& record) {
    return record.avRecordSignature == PseudoSha256(RecordBytesForSignature(record));
}

AvRecord MakeRecord(const char* signature, ObjectType type, unsigned long long offsetBegin, unsigned long long offsetEnd, const wchar_t* name) {
    const size_t length = strlen(signature);
    std::vector<unsigned char> bytes(signature, signature + length);
    AvRecord record;
    record.objectSignaturePrefix = ReadLe64(bytes.data());
    record.objectSignatureLength = static_cast<unsigned int>(bytes.size());
    record.objectSignature = PseudoSha256(bytes);
    record.offsetBegin = offsetBegin;
    record.offsetEnd = offsetEnd;
    record.objectType = type;
    record.threatName = name;
    record.avRecordSignature = PseudoSha256(RecordBytesForSignature(record));
    return record;
}

void BuildAhoTrieLocked() {
    g_ahoTrie.clear();
    g_ahoTrie.push_back(AhoNode{});
    for (const auto& bucket : g_avDatabase.recordsByPrefix) {
        std::vector<unsigned char> prefix = Le64Bytes(bucket.first);
        int node = 0;
        for (unsigned char byte : prefix) {
            auto it = g_ahoTrie[static_cast<size_t>(node)].next.find(byte);
            if (it == g_ahoTrie[static_cast<size_t>(node)].next.end()) {
                g_ahoTrie[static_cast<size_t>(node)].next[byte] = static_cast<int>(g_ahoTrie.size());
                g_ahoTrie.push_back(AhoNode{});
                node = static_cast<int>(g_ahoTrie.size() - 1);
            } else {
                node = it->second;
            }
        }
        g_ahoTrie[static_cast<size_t>(node)].prefixes.push_back(bucket.first);
    }

    std::queue<int> queue;
    for (const auto& edge : g_ahoTrie[0].next) queue.push(edge.second);
    while (!queue.empty()) {
        int vertex = queue.front();
        queue.pop();
        for (const auto& edge : g_ahoTrie[static_cast<size_t>(vertex)].next) {
            unsigned char byte = edge.first;
            int child = edge.second;
            int link = g_ahoTrie[static_cast<size_t>(vertex)].link;
            while (link != 0 && !g_ahoTrie[static_cast<size_t>(link)].next.count(byte)) {
                link = g_ahoTrie[static_cast<size_t>(link)].link;
            }
            if (g_ahoTrie[static_cast<size_t>(link)].next.count(byte)) {
                link = g_ahoTrie[static_cast<size_t>(link)].next[byte];
            }
            g_ahoTrie[static_cast<size_t>(child)].link = link;
            auto& childPrefixes = g_ahoTrie[static_cast<size_t>(child)].prefixes;
            const auto& linkPrefixes = g_ahoTrie[static_cast<size_t>(link)].prefixes;
            childPrefixes.insert(childPrefixes.end(), linkPrefixes.begin(), linkPrefixes.end());
            queue.push(child);
        }
    }
}

bool LoadAvDatabase() {
    AvDatabase database;
    database.releaseDate = L"2026-05-09";
    std::vector<AvRecord> records = {
        MakeRecord("MZ.ZIOVPO.EICAR.PE", ObjectType::PeFile, 0, 4096, L"Demo.PE.Ziovpo"),
        MakeRecord("# ZIOVPO-TEST-SCRIPT", ObjectType::Script, 0, 1024 * 1024, L"Demo.Script.Ziovpo")
    };

    for (const auto& record : records) {
        if (record.objectSignatureLength < 8 || !VerifyRecordSignature(record)) {
            return false;
        }
        database.recordsByPrefix[record.objectSignaturePrefix].push_back(record);
    }
    database.loaded = true;

    EnterCriticalSection(&g_avLock);
    g_avDatabase = database;
    BuildAhoTrieLocked();
    LeaveCriticalSection(&g_avLock);
    return true;
}

long AvRecordCountLocked() {
    long count = 0;
    for (const auto& bucket : g_avDatabase.recordsByPrefix) count += static_cast<long>(bucket.second.size());
    return count;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::wstring FileExtension(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    return ToLower(path.substr(dot));
}

ObjectType DetectObjectType(const std::wstring& path, const std::vector<unsigned char>& data) {
    const std::wstring ext = FileExtension(path);
    if (ext == L".ps1" || ext == L".js" || ext == L".py" || ext == L".vbs" || ext == L".bat" || ext == L".cmd") {
        return ObjectType::Script;
    }
    if (data.size() >= 2 && data[0] == 'M' && data[1] == 'Z') return ObjectType::PeFile;
    if (ext == L".exe" || ext == L".dll" || ext == L".sys") return ObjectType::PeFile;
    return ObjectType::Script;
}

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>* bytes) {
    if (!bytes) return false;
    bytes->clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0 || size > 256LL * 1024LL * 1024LL) return false;
    stream.seekg(0, std::ios::beg);
    bytes->resize(static_cast<size_t>(size));
    if (!bytes->empty()) stream.read(reinterpret_cast<char*>(bytes->data()), size);
    return stream.good() || stream.eof();
}

struct ByteStream {
    virtual ~ByteStream() = default;
    virtual bool ReadAll(std::vector<unsigned char>* bytes) = 0;
};

struct FileByteStream final : ByteStream {
    explicit FileByteStream(std::wstring value) : path(std::move(value)) {}
    bool ReadAll(std::vector<unsigned char>* bytes) override { return ReadFileBytes(path, bytes); }
    std::wstring path;
};

void MarkScanError(ScanResult* result, long errorCode, const std::wstring& message) {
    if (!result) return;
    result->errorCode = errorCode;
    CopyString(result->message, std::size(result->message), message);
}

void MergeScanResult(ScanResult* target, const ScanResult& item) {
    if (!target) return;
    target->scannedFiles += item.scannedFiles;
    target->infectedFiles += item.infectedFiles;
    if (target->errorCode == 0 && item.errorCode != 0) target->errorCode = item.errorCode;
    if (target->infectedFiles == item.infectedFiles && item.infectedFiles > 0 && target->objectPath[0] == L'\0') {
        CopyString(target->objectPath, std::size(target->objectPath), item.objectPath);
        CopyString(target->threatName, std::size(target->threatName), item.threatName);
    }
}

bool ScanBufferWithDatabase(const std::wstring& path, const std::vector<unsigned char>& data, ScanResult* result) {
    if (!result) return false;

    AvDatabase database;
    std::vector<AhoNode> trie;
    EnterCriticalSection(&g_avLock);
    database = g_avDatabase;
    trie = g_ahoTrie;
    LeaveCriticalSection(&g_avLock);

    if (!database.loaded || trie.empty()) {
        MarkScanError(result, kErrorAvDatabaseNotLoaded, L"Антивирусные базы не загружены");
        return false;
    }

    result->scannedFiles = 1;
    const ObjectType objectType = DetectObjectType(path, data);
    int node = 0;
    for (size_t pos = 0; pos < data.size(); ++pos) {
        const unsigned char byte = data[pos];
        while (node != 0 && !trie[static_cast<size_t>(node)].next.count(byte)) {
            node = trie[static_cast<size_t>(node)].link;
        }
        auto nextIt = trie[static_cast<size_t>(node)].next.find(byte);
        if (nextIt != trie[static_cast<size_t>(node)].next.end()) node = nextIt->second;

        for (unsigned long long prefix : trie[static_cast<size_t>(node)].prefixes) {
            if (pos + 1 < 8) continue;
            const unsigned long long offset = static_cast<unsigned long long>(pos + 1 - 8);
            auto bucketIt = database.recordsByPrefix.find(prefix);
            if (bucketIt == database.recordsByPrefix.end()) continue;

            for (const AvRecord& record : bucketIt->second) {
                if (record.objectType != objectType) continue;
                if (offset < record.offsetBegin || offset > record.offsetEnd) continue;
                if (record.objectSignatureLength < 8) continue;
                const size_t length = static_cast<size_t>(record.objectSignatureLength);
                if (offset + length > data.size()) continue;

                std::vector<unsigned char> signatureBytes(data.begin() + static_cast<ptrdiff_t>(offset),
                                                          data.begin() + static_cast<ptrdiff_t>(offset + length));
                if (PseudoSha256(signatureBytes) == record.objectSignature) {
                    result->infectedFiles = 1;
                    result->errorCode = 0;
                    CopyString(result->objectPath, std::size(result->objectPath), path);
                    CopyString(result->threatName, std::size(result->threatName), record.threatName);
                    CopyString(result->message, std::size(result->message), L"Обнаружен вредоносный объект");
                    return true;
                }
            }
        }
    }

    CopyString(result->message, std::size(result->message), L"Угрозы не обнаружены");
    return false;
}

bool ScanSingleFile(const std::wstring& path, ScanResult* result) {
    if (!result) return false;
    ZeroMemory(result, sizeof(*result));
    std::vector<unsigned char> bytes;
    FileByteStream stream(path);
    if (!stream.ReadAll(&bytes)) {
        MarkScanError(result, kErrorScanFailed, L"Не удалось прочитать файл");
        return false;
    }
    return ScanBufferWithDatabase(path, bytes, result);
}

void ScanDirectoryRecursive(const std::wstring& path, ScanResult* result) {
    if (!result) return;
    std::wstring mask = path;
    if (!mask.empty() && mask.back() != L'\\' && mask.back() != L'/') mask += L"\\";
    mask += L"*";

    WIN32_FIND_DATAW data = {};
    HANDLE findHandle = FindFirstFileW(mask.c_str(), &data);
    if (findHandle == INVALID_HANDLE_VALUE) {
        MarkScanError(result, kErrorScanFailed, L"Не удалось открыть директорию");
        return;
    }

    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        std::wstring child = path;
        if (!child.empty() && child.back() != L'\\' && child.back() != L'/') child += L"\\";
        child += data.cFileName;

        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDirectoryRecursive(child, result);
            if (result->infectedFiles > 0) break;
        } else {
            ScanResult item = {};
            ScanSingleFile(child, &item);
            MergeScanResult(result, item);
            if (item.infectedFiles > 0) break;
        }
    } while (FindNextFileW(findHandle, &data));

    FindClose(findHandle);
    if (result->message[0] == L'\0') {
        CopyString(result->message, std::size(result->message),
                   result->infectedFiles > 0 ? L"Обнаружен вредоносный объект" : L"Угрозы не обнаружены");
    }
}

void RunConfiguredScan(const std::wstring& path) {
    ScanResult ignored = {};
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        ScanDirectoryRecursive(path, &ignored);
    } else {
        ScanSingleFile(path, &ignored);
    }
}

DWORD WINAPI MonitorThreadProc(LPVOID) {
    for (;;) {
        if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) return 0;

        std::wstring path;
        bool enabled = false;
        EnterCriticalSection(&g_avLock);
        enabled = g_monitorEnabled;
        path = g_monitorPath;
        LeaveCriticalSection(&g_avLock);
        if (!enabled || path.empty()) continue;

        HANDLE dir = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dir == INVALID_HANDLE_VALUE) {
            Sleep(3000);
            continue;
        }

        BYTE buffer[4096] = {};
        DWORD bytesReturned = 0;
        BOOL ok = ReadDirectoryChangesW(dir, buffer, sizeof(buffer), TRUE,
                                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                        &bytesReturned, nullptr, nullptr);
        if (ok && bytesReturned >= sizeof(FILE_NOTIFY_INFORMATION)) {
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
            std::wstring fileName(info->FileName, info->FileNameLength / sizeof(wchar_t));
            std::wstring changedPath = path;
            if (!changedPath.empty() && changedPath.back() != L'\\' && changedPath.back() != L'/') changedPath += L"\\";
            changedPath += fileName;
            if (!(GetFileAttributesW(changedPath.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) RunConfiguredScan(changedPath);
        }
        CloseHandle(dir);
    }
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

std::vector<std::wstring> SplitServerUrls(const std::wstring& value) {
    std::vector<std::wstring> urls;
    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find_first_of(L";,", start);
        std::wstring item = value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        while (!item.empty() && iswspace(item.front())) item.erase(item.begin());
        while (!item.empty() && iswspace(item.back())) item.pop_back();
        if (!item.empty()) urls.push_back(item);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return urls;
}

std::wstring JoinServerUrls(const std::vector<std::wstring>& urls) {
    std::wstring result;
    for (const auto& url : urls) {
        if (!result.empty()) result += L", ";
        result += url;
    }
    return result;
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

HttpResponse HttpPostWithFallback(const wchar_t* path, const std::string& body, const std::string& bearerToken = {}) {
    std::vector<std::wstring> urls;
    EnterCriticalSection(&g_stateLock);
    urls = g_state.serverUrls;
    if (urls.empty()) urls.push_back(g_state.serverUrl);
    LeaveCriticalSection(&g_stateLock);

    HttpResponse lastResponse;
    for (const auto& url : urls) {
        HttpResponse response = HttpPost(url, path, body, bearerToken);
        if (response.status != 0) {
            EnterCriticalSection(&g_stateLock);
            g_state.serverUrl = url;
            LeaveCriticalSection(&g_stateLock);
            return response;
        }
        lastResponse = response;
    }
    return lastResponse;
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

std::wstring ResponseErrorMessage(const HttpResponse& response, const wchar_t* fallback) {
    if (response.status == 0) return L"Сервер недоступен";
    const std::string error = JsonString(response.body, "error");
    if (!error.empty()) return Utf8ToWide(error);
    return fallback;
}

bool RefreshTokens() {
    std::wstring email;
    std::string refreshToken;
    EnterCriticalSection(&g_stateLock);
    email = g_state.email;
    refreshToken = g_state.refreshToken;
    LeaveCriticalSection(&g_stateLock);
    if (refreshToken.empty()) return false;

    const std::string body = "{\"refreshToken\":\"" + JsonEscape(refreshToken) + "\"}";
    const HttpResponse response = HttpPostWithFallback(L"/auth/refresh", body);
    EnterCriticalSection(&g_stateLock);
    const bool ok = response.status == 200 && StoreTokenResponseLocked(email, response.body);
    if (!ok) {
        ClearAuthLocked();
        g_state.lastAuthError = L"Сессия истекла. Войдите заново.";
    }
    LeaveCriticalSection(&g_stateLock);
    if (ok) LoadAvDatabase();
    SetEvent(g_stateChangedEvent);
    return ok;
}

bool CheckLicense() {
    std::string accessToken;
    long productId = 1;
    std::wstring mac = GetDeviceMac();
    EnterCriticalSection(&g_stateLock);
    accessToken = g_state.accessToken;
    productId = g_state.productId;
    LeaveCriticalSection(&g_stateLock);
    if (accessToken.empty()) return false;

    std::ostringstream body;
    body << "{\"deviceMac\":\"" << JsonEscape(WideToUtf8(mac)) << "\",\"productId\":" << productId << "}";
    const HttpResponse response = HttpPostWithFallback(L"/api/licenses/check", body.str(), accessToken);
    EnterCriticalSection(&g_stateLock);
    const bool ok = response.status == 200 && StoreTicketResponseLocked(response.body);
    if (!ok) ClearLicenseLocked(ResponseErrorMessage(response, L"Активная лицензия не найдена"));
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
    EnterCriticalSection(&g_avLock);
    if (g_scheduleEnabled) {
        waitMs = static_cast<DWORD>(g_nextScheduledScan > NowMs()
            ? std::min<ULONGLONG>(g_nextScheduledScan - NowMs(), waitMs == INFINITE ? g_nextScheduledScan - NowMs() : waitMs)
            : 0);
    }
    LeaveCriticalSection(&g_avLock);
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
        bool needScheduledScan = false;
        std::wstring scheduledPath;
        EnterCriticalSection(&g_stateLock);
        needTokenRefresh = !g_state.refreshToken.empty() &&
                           ((g_state.accessExpiry > 0 && g_state.accessExpiry <= NowMs() + 60000) ||
                            (g_state.refreshExpiry > 0 && g_state.refreshExpiry <= NowMs() + 60000));
        needLicenseRefresh = !g_state.licenseTicketJson.empty() && g_state.licenseRefreshAt <= NowMs();
        LeaveCriticalSection(&g_stateLock);

        EnterCriticalSection(&g_avLock);
        if (g_scheduleEnabled && !g_schedulePath.empty() && g_nextScheduledScan <= NowMs()) {
            needScheduledScan = true;
            scheduledPath = g_schedulePath;
            g_nextScheduledScan = NowMs() + static_cast<ULONGLONG>(std::max<DWORD>(g_scheduleIntervalMinutes, 1)) * 60ULL * 1000ULL;
        }
        LeaveCriticalSection(&g_avLock);

        if (needTokenRefresh) RefreshTokens();
        if (needLicenseRefresh) CheckLicense();
        if (needScheduledScan) RunConfiguredScan(scheduledPath);
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
    g_serviceStatus.dwControlsAccepted = (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SESSIONCHANGE) : 0;
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
    std::wstring configuredUrls = ReadEnvString(L"TRAYAPP_SERVER_URLS", L"");
    if (configuredUrls.empty()) configuredUrls = ReadEnvString(L"TRAYAPP_SERVER_URL", kDefaultServerUrls);
    g_state.serverUrls = SplitServerUrls(configuredUrls);
    if (g_state.serverUrls.empty()) g_state.serverUrls = SplitServerUrls(kDefaultServerUrls);
    g_state.serverUrl = g_state.serverUrls.front();
    g_state.productId = ReadEnvLong(L"TRAYAPP_PRODUCT_ID", 1);

    RPC_STATUS status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)kRpcEndpoint, nullptr);
    if (status == RPC_S_OK) status = RpcServerRegisterIf(ITrayService_v1_0_s_ifspec, nullptr, nullptr);
    if (status == RPC_S_OK) status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    if (status != RPC_S_OK) {
        UpdateServiceState(SERVICE_STOPPED, status);
        return status;
    }

    g_backgroundThread = CreateThread(nullptr, 0, BackgroundWorkerThread, nullptr, 0, nullptr);
    g_monitorThread = CreateThread(nullptr, 0, MonitorThreadProc, nullptr, 0, nullptr);
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
    if (g_monitorThread) {
        WaitForSingleObject(g_monitorThread, 5000);
        CloseHandle(g_monitorThread);
        g_monitorThread = nullptr;
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

    const std::string body = "{\"email\":\"" + JsonEscape(WideToUtf8(email)) + "\",\"password\":\"" + JsonEscape(WideToUtf8(password)) + "\"}";
    const HttpResponse response = HttpPostWithFallback(L"/auth/login", body);

    EnterCriticalSection(&g_stateLock);
    if (response.status == 200 && StoreTokenResponseLocked(email, response.body)) {
        ClearLicenseLocked();
        info->authenticated = 1;
        info->errorCode = 0;
        CopyString(info->email, std::size(info->email), g_state.email);
        CopyString(info->message, std::size(info->message), L"Вход выполнен");
    } else {
        ClearAuthLocked();
        g_state.lastAuthError = response.status == 0
            ? L"Сервер недоступен. Проверены адреса: " + JoinServerUrls(g_state.serverUrls)
            : L"Неверный email или пароль";
        info->authenticated = 0;
        info->errorCode = response.status == 0 ? kErrorNetwork : kErrorBadResponse;
        CopyString(info->message, std::size(info->message), g_state.lastAuthError);
    }
    LeaveCriticalSection(&g_stateLock);
    SetEvent(g_stateChangedEvent);
    return RPC_S_OK;
}

error_status_t Logout() {
    std::string refreshToken;
    EnterCriticalSection(&g_stateLock);
    refreshToken = g_state.refreshToken;
    LeaveCriticalSection(&g_stateLock);
    if (!refreshToken.empty()) {
        const std::string body = "{\"refreshToken\":\"" + JsonEscape(refreshToken) + "\"}";
        HttpPostWithFallback(L"/auth/logout", body);
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
    std::string accessToken;
    EnterCriticalSection(&g_stateLock);
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
    const HttpResponse response = HttpPostWithFallback(L"/api/licenses/activate", body, accessToken);
    EnterCriticalSection(&g_stateLock);
    bool ok = response.status == 200 && StoreTicketResponseLocked(response.body);
    if (!ok) {
        ClearLicenseLocked(ResponseErrorMessage(response, L"Не удалось активировать продукт"));
        info->hasLicense = 0;
        info->blocked = 0;
        info->errorCode = response.status == 0 ? kErrorNetwork : kErrorBadResponse;
        CopyString(info->message, std::size(info->message), g_state.lastLicenseError);
    }
    LeaveCriticalSection(&g_stateLock);
    if (!ok) {
        SetEvent(g_stateChangedEvent);
        return RPC_S_OK;
    }
    LoadAvDatabase();
    SetEvent(g_stateChangedEvent);
    return GetLicenseInfo(info);
}

error_status_t GetAvDatabaseInfo(AvDatabaseInfo* info) {
    if (!info) return RPC_X_NULL_REF_POINTER;
    ZeroMemory(info, sizeof(*info));
    EnterCriticalSection(&g_avLock);
    info->loaded = g_avDatabase.loaded ? 1 : 0;
    info->recordCount = AvRecordCountLocked();
    CopyString(info->releaseDate, std::size(info->releaseDate), g_avDatabase.releaseDate);
    CopyString(info->message, std::size(info->message),
               g_avDatabase.loaded ? L"Антивирусные базы загружены" : L"Антивирусные базы не загружены");
    LeaveCriticalSection(&g_avLock);
    return RPC_S_OK;
}

error_status_t ScanFile(wchar_t* path, ScanResult* result) {
    if (!path || !result) return RPC_X_NULL_REF_POINTER;
    ScanSingleFile(path, result);
    return RPC_S_OK;
}

error_status_t ScanDirectory(wchar_t* path, ScanResult* result) {
    if (!path || !result) return RPC_X_NULL_REF_POINTER;
    ZeroMemory(result, sizeof(*result));
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        MarkScanError(result, kErrorScanFailed, L"Директория не найдена");
        return RPC_S_OK;
    }
    ScanDirectoryRecursive(path, result);
    return RPC_S_OK;
}

error_status_t ScanFixedDrives(ScanResult* result) {
    if (!result) return RPC_X_NULL_REF_POINTER;
    ZeroMemory(result, sizeof(*result));
    DWORD mask = GetLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if ((mask & (1u << (letter - L'A'))) == 0) continue;
        wchar_t root[] = { letter, L':', L'\\', L'\0' };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ScanDirectoryRecursive(root, result);
        if (result->infectedFiles > 0) break;
    }
    if (result->message[0] == L'\0') {
        CopyString(result->message, std::size(result->message), L"Угрозы не обнаружены");
    }
    return RPC_S_OK;
}

error_status_t ConfigureScheduledScan(long enabled, long intervalMinutes, wchar_t* path, wchar_t message[512]) {
    if (!path || !message) return RPC_X_NULL_REF_POINTER;
    EnterCriticalSection(&g_avLock);
    g_scheduleEnabled = enabled != 0;
    g_scheduleIntervalMinutes = static_cast<DWORD>(std::max<long>(intervalMinutes, 1));
    g_schedulePath = path;
    g_nextScheduledScan = NowMs() + static_cast<ULONGLONG>(g_scheduleIntervalMinutes) * 60ULL * 1000ULL;
    LeaveCriticalSection(&g_avLock);
    SetEvent(g_stateChangedEvent);
    CopyString(message, 512, g_scheduleEnabled ? L"Сканирование по расписанию включено" : L"Сканирование по расписанию выключено");
    return RPC_S_OK;
}

error_status_t ConfigureDirectoryMonitoring(long enabled, wchar_t* path, wchar_t message[512]) {
    if (!path || !message) return RPC_X_NULL_REF_POINTER;
    EnterCriticalSection(&g_avLock);
    g_monitorEnabled = enabled != 0;
    g_monitorPath = path;
    LeaveCriticalSection(&g_avLock);
    CopyString(message, 512, g_monitorEnabled ? L"Мониторинг директории включен" : L"Мониторинг директории выключен");
    return RPC_S_OK;
}
}

void* __RPC_USER MIDL_user_allocate(size_t s) { return malloc(s); }
void __RPC_USER MIDL_user_free(void* p) { free(p); }

DWORD WINAPI ServiceControlHandlerEx(DWORD ctrl, DWORD evtType, LPVOID evtData, LPVOID) {
    if (ctrl == SERVICE_CONTROL_STOP) {
        UpdateServiceState(SERVICE_STOP_PENDING);
        if (g_stopEvent) SetEvent(g_stopEvent);
        return NO_ERROR;
    }

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
    InitializeCriticalSection(&g_avLock);
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(kServiceName), ServiceMain }, { nullptr, nullptr } };
    StartServiceCtrlDispatcherW(table);
    DeleteCriticalSection(&g_avLock);
    DeleteCriticalSection(&g_stateLock);
    DeleteCriticalSection(&g_processesLock);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) { return wmain(); }
