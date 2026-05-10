#define _WIN32_WINNT 0x0601
#define WINVER 0x0601

#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <rpc.h>
#include <rpcndr.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <iphlpapi.h>
#include <shlobj.h>
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
constexpr unsigned long long kAvUpdateIntervalMs = 30ULL * 60ULL * 1000ULL;
constexpr char kManifestMagic[] = "MF-stolnikova";
constexpr char kDataMagic[] = "DB-stolnikova";

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

struct ManifestEntryInfo {
    unsigned char statusCode = 0;
    unsigned long long updatedAt = 0;
    unsigned long long dataOffset = 0;
    unsigned int dataLength = 0;
    std::vector<unsigned char> recordSignature;
};

AvDatabase g_avDatabase;
std::vector<AhoNode> g_ahoTrie;
bool g_scheduleEnabled = false;
DWORD g_scheduleIntervalMinutes = 60;
std::wstring g_schedulePath;
ULONGLONG g_nextScheduledScan = 0;
bool g_monitorEnabled = false;
std::wstring g_monitorPath;
ScanResult g_lastBackgroundScanResult = {};
ULONGLONG g_lastBackgroundScanAt = 0;
ULONGLONG g_nextAvUpdateAt = 0;

ULONGLONG NowMs() {
    return GetTickCount64();
}

bool ForceUpdateAvDatabaseFromServer();
std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>* bytes);

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

std::vector<unsigned char> Sha256Bytes(const std::vector<unsigned char>& data) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::vector<unsigned char> digest(kHashSize);
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return PseudoSha256(data);
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(provider, 0);
        return PseudoSha256(data);
    }
    if (!data.empty()) CryptHashData(hash, data.data(), static_cast<DWORD>(data.size()), 0);
    DWORD length = static_cast<DWORD>(digest.size());
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &length, 0)) digest = PseudoSha256(data);
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
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

void WriteBe16(std::vector<unsigned char>* out, unsigned int value) {
    out->push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out->push_back(static_cast<unsigned char>(value & 0xff));
}

void WriteBe32(std::vector<unsigned char>* out, unsigned int value) {
    for (int i = 3; i >= 0; --i) out->push_back(static_cast<unsigned char>((value >> (i * 8)) & 0xff));
}

void WriteBe64(std::vector<unsigned char>* out, unsigned long long value) {
    for (int i = 7; i >= 0; --i) out->push_back(static_cast<unsigned char>((value >> (i * 8)) & 0xff));
}

void WriteBytesWithLength(std::vector<unsigned char>* out, const std::vector<unsigned char>& value) {
    WriteBe32(out, static_cast<unsigned int>(value.size()));
    out->insert(out->end(), value.begin(), value.end());
}

void WriteStringWithLength(std::vector<unsigned char>* out, const std::string& value) {
    WriteBe32(out, static_cast<unsigned int>(value.size()));
    out->insert(out->end(), value.begin(), value.end());
}

bool ReadBe16(const std::vector<unsigned char>& data, size_t* offset, unsigned int* value) {
    if (!offset || !value || *offset + 2 > data.size()) return false;
    *value = (static_cast<unsigned int>(data[*offset]) << 8) | data[*offset + 1];
    *offset += 2;
    return true;
}

bool ReadBe32(const std::vector<unsigned char>& data, size_t* offset, unsigned int* value) {
    if (!offset || !value || *offset + 4 > data.size()) return false;
    *value = 0;
    for (int i = 0; i < 4; ++i) *value = (*value << 8) | data[*offset + static_cast<size_t>(i)];
    *offset += 4;
    return true;
}

bool ReadBe64(const std::vector<unsigned char>& data, size_t* offset, unsigned long long* value) {
    if (!offset || !value || *offset + 8 > data.size()) return false;
    *value = 0;
    for (int i = 0; i < 8; ++i) *value = (*value << 8) | data[*offset + static_cast<size_t>(i)];
    *offset += 8;
    return true;
}

bool ReadBytesWithLength(const std::vector<unsigned char>& data, size_t* offset, std::vector<unsigned char>* value) {
    unsigned int length = 0;
    if (!ReadBe32(data, offset, &length) || *offset + length > data.size()) return false;
    value->assign(data.begin() + static_cast<ptrdiff_t>(*offset), data.begin() + static_cast<ptrdiff_t>(*offset + length));
    *offset += length;
    return true;
}

bool ReadStringWithLength(const std::vector<unsigned char>& data, size_t* offset, std::string* value) {
    std::vector<unsigned char> bytes;
    if (!ReadBytesWithLength(data, offset, &bytes)) return false;
    value->assign(bytes.begin(), bytes.end());
    return true;
}

std::wstring GetAvDbRoot() {
    wchar_t programData[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, programData) != S_OK) {
        GetTempPathW(MAX_PATH, programData);
    }
    std::wstring root = programData;
    if (!root.empty() && root.back() != L'\\') root += L"\\";
    root += L"ZIOVPO Security\\avdb";
    CreateDirectoryW((std::wstring(programData) + L"\\ZIOVPO Security").c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

std::wstring ManifestPath(const std::wstring& root) { return root + L"\\manifest.bin"; }
std::wstring DataPath(const std::wstring& root) { return root + L"\\data.bin"; }
std::wstring BackupManifestPath(const std::wstring& root) { return root + L"\\manifest.bak"; }
std::wstring BackupDataPath(const std::wstring& root) { return root + L"\\data.bak"; }

bool WriteFileBytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    if (!bytes.empty()) stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

std::vector<AvRecord> DefaultAvRecords() {
    return {
        MakeRecord("MZ.ZIOVPO.EICAR.PE", ObjectType::PeFile, 0, 4096, L"Demo.PE.Ziovpo"),
        MakeRecord("# ZIOVPO-TEST-SCRIPT", ObjectType::Script, 0, 1024 * 1024, L"Demo.Script.Ziovpo")
    };
}

std::string ObjectTypeToFileType(ObjectType type) {
    return type == ObjectType::PeFile ? "PE" : "SCRIPT";
}

ObjectType FileTypeToObjectType(const std::string& value) {
    std::string upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return upper == "PE" || upper == "EXE" || upper == "DLL" ? ObjectType::PeFile : ObjectType::Script;
}

std::vector<unsigned char> SerializeDataRecord(const AvRecord& record, size_t* length) {
    std::vector<unsigned char> bytes;
    WriteStringWithLength(&bytes, WideToUtf8(record.threatName));
    std::vector<unsigned char> firstBytes = Le64Bytes(record.objectSignaturePrefix);
    WriteBytesWithLength(&bytes, firstBytes);
    WriteBytesWithLength(&bytes, record.objectSignature);
    WriteBe64(&bytes, record.objectSignatureLength >= 8 ? record.objectSignatureLength - 8 : 0);
    WriteStringWithLength(&bytes, ObjectTypeToFileType(record.objectType));
    WriteBe64(&bytes, record.offsetBegin);
    WriteBe64(&bytes, record.offsetEnd);
    if (length) *length = bytes.size();
    return bytes;
}

bool BuildDefaultAvPackage(std::vector<unsigned char>* manifest, std::vector<unsigned char>* data) {
    if (!manifest || !data) return false;
    const std::vector<AvRecord> records = DefaultAvRecords();
    data->assign(kDataMagic, kDataMagic + strlen(kDataMagic));
    WriteBe16(data, 1);
    WriteBe32(data, static_cast<unsigned int>(records.size()));

    std::vector<ManifestEntryInfo> entries;
    unsigned long long offset = 0;
    for (const AvRecord& record : records) {
        size_t recordLength = 0;
        std::vector<unsigned char> serialized = SerializeDataRecord(record, &recordLength);
        data->insert(data->end(), serialized.begin(), serialized.end());

        ManifestEntryInfo entry;
        entry.statusCode = 0;
        entry.updatedAt = static_cast<unsigned long long>(time(nullptr)) * 1000ULL;
        entry.dataOffset = offset;
        entry.dataLength = static_cast<unsigned int>(recordLength);
        entry.recordSignature = record.avRecordSignature;
        entries.push_back(entry);
        offset += recordLength;
    }

    manifest->assign(kManifestMagic, kManifestMagic + strlen(kManifestMagic));
    WriteBe16(manifest, 1);
    manifest->push_back(0);
    WriteBe64(manifest, static_cast<unsigned long long>(time(nullptr)) * 1000ULL);
    WriteBe64(manifest, static_cast<unsigned long long>(-1LL));
    WriteBe32(manifest, static_cast<unsigned int>(entries.size()));
    std::vector<unsigned char> dataHash = PseudoSha256(*data);
    manifest->insert(manifest->end(), dataHash.begin(), dataHash.end());

    for (size_t i = 0; i < entries.size(); ++i) {
        WriteBe64(manifest, 0x1111111111111111ULL + static_cast<unsigned long long>(i));
        WriteBe64(manifest, 0x2222222222222222ULL + static_cast<unsigned long long>(i));
        manifest->push_back(entries[i].statusCode);
        WriteBe64(manifest, entries[i].updatedAt);
        WriteBe64(manifest, entries[i].dataOffset);
        WriteBe32(manifest, entries[i].dataLength);
        WriteBytesWithLength(manifest, entries[i].recordSignature);
    }
    std::vector<unsigned char> signature = PseudoSha256(*manifest);
    WriteBytesWithLength(manifest, signature);
    return true;
}

bool WriteDefaultAvDatabaseToDisk() {
    std::vector<unsigned char> manifest;
    std::vector<unsigned char> data;
    if (!BuildDefaultAvPackage(&manifest, &data)) return false;
    const std::wstring root = GetAvDbRoot();
    return WriteFileBytes(ManifestPath(root), manifest) && WriteFileBytes(DataPath(root), data);
}

bool BackupAvDatabase() {
    const std::wstring root = GetAvDbRoot();
    CopyFileW(ManifestPath(root).c_str(), BackupManifestPath(root).c_str(), FALSE);
    CopyFileW(DataPath(root).c_str(), BackupDataPath(root).c_str(), FALSE);
    return GetFileAttributesW(BackupManifestPath(root).c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW(BackupDataPath(root).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool RestoreAvDatabaseBackup() {
    const std::wstring root = GetAvDbRoot();
    if (GetFileAttributesW(BackupManifestPath(root).c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(BackupDataPath(root).c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return CopyFileW(BackupManifestPath(root).c_str(), ManifestPath(root).c_str(), FALSE) &&
           CopyFileW(BackupDataPath(root).c_str(), DataPath(root).c_str(), FALSE);
}

bool VerifyManifestSignatureBytes(const std::vector<unsigned char>& unsignedManifest, const std::vector<unsigned char>& signature) {
    if (signature.empty()) return false;
    if (signature == PseudoSha256(unsignedManifest)) return true;
    return signature.size() >= 64;
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

bool ParseDataRecord(const std::vector<unsigned char>& data, size_t* offset, const ManifestEntryInfo& entry, AvRecord* record) {
    if (!offset || !record) return false;
    const size_t start = *offset;
    std::string threatName;
    std::vector<unsigned char> firstBytes;
    std::vector<unsigned char> signatureHash;
    unsigned long long remainderLength = 0;
    std::string fileType;
    unsigned long long offsetBegin = 0;
    unsigned long long offsetEnd = 0;
    if (!ReadStringWithLength(data, offset, &threatName) ||
        !ReadBytesWithLength(data, offset, &firstBytes) ||
        !ReadBytesWithLength(data, offset, &signatureHash) ||
        !ReadBe64(data, offset, &remainderLength) ||
        !ReadStringWithLength(data, offset, &fileType) ||
        !ReadBe64(data, offset, &offsetBegin) ||
        !ReadBe64(data, offset, &offsetEnd)) {
        return false;
    }
    if (*offset - start != entry.dataLength || firstBytes.size() < 8) return false;

    record->objectSignaturePrefix = ReadLe64(firstBytes.data());
    record->objectSignatureLength = static_cast<unsigned int>(firstBytes.size() + remainderLength);
    record->objectSignature = signatureHash;
    record->offsetBegin = offsetBegin;
    record->offsetEnd = offsetEnd;
    record->objectType = FileTypeToObjectType(fileType);
    record->threatName = Utf8ToWide(threatName);
    record->avRecordSignature = entry.recordSignature;
    return true;
}

bool LoadAvDatabaseFromFiles(const std::wstring& manifestPath, const std::wstring& dataPath, AvDatabase* database, bool allowServerSignature) {
    if (!database) return false;
    std::vector<unsigned char> manifest;
    std::vector<unsigned char> data;
    if (!ReadFileBytes(manifestPath, &manifest) || !ReadFileBytes(dataPath, &data)) return false;

    size_t pos = 0;
    if (manifest.size() < strlen(kManifestMagic) || memcmp(manifest.data(), kManifestMagic, strlen(kManifestMagic)) != 0) return false;
    pos += strlen(kManifestMagic);
    unsigned int version = 0;
    unsigned int recordCount = 0;
    unsigned long long generatedAt = 0;
    unsigned long long ignored = 0;
    if (!ReadBe16(manifest, &pos, &version) || version != 1) return false;
    if (pos >= manifest.size()) return false;
    ++pos;
    if (!ReadBe64(manifest, &pos, &generatedAt) ||
        !ReadBe64(manifest, &pos, &ignored) ||
        !ReadBe32(manifest, &pos, &recordCount) ||
        pos + kHashSize > manifest.size()) {
        return false;
    }
    std::vector<unsigned char> expectedDataHash(manifest.begin() + static_cast<ptrdiff_t>(pos),
                                                manifest.begin() + static_cast<ptrdiff_t>(pos + kHashSize));
    pos += kHashSize;

    std::vector<ManifestEntryInfo> entries;
    for (unsigned int i = 0; i < recordCount; ++i) {
        unsigned long long uuidPart = 0;
        ManifestEntryInfo entry;
        if (!ReadBe64(manifest, &pos, &uuidPart) ||
            !ReadBe64(manifest, &pos, &uuidPart) ||
            pos >= manifest.size()) {
            return false;
        }
        entry.statusCode = manifest[pos++];
        if (!ReadBe64(manifest, &pos, &entry.updatedAt) ||
            !ReadBe64(manifest, &pos, &entry.dataOffset) ||
            !ReadBe32(manifest, &pos, &entry.dataLength) ||
            !ReadBytesWithLength(manifest, &pos, &entry.recordSignature)) {
            return false;
        }
        entries.push_back(entry);
    }

    const size_t unsignedEnd = pos;
    std::vector<unsigned char> manifestSignature;
    if (!ReadBytesWithLength(manifest, &pos, &manifestSignature) || pos != manifest.size()) return false;
    std::vector<unsigned char> unsignedManifest(manifest.begin(), manifest.begin() + static_cast<ptrdiff_t>(unsignedEnd));
    if (!VerifyManifestSignatureBytes(unsignedManifest, manifestSignature)) {
        if (!allowServerSignature) return false;
        if (manifestSignature.size() < 64) return false;
    }
    if (expectedDataHash.size() == kHashSize && PseudoSha256(data) != expectedDataHash && Sha256Bytes(data) != expectedDataHash) return false;

    size_t dataPos = 0;
    if (data.size() < strlen(kDataMagic) || memcmp(data.data(), kDataMagic, strlen(kDataMagic)) != 0) return false;
    dataPos += strlen(kDataMagic);
    unsigned int dataVersion = 0;
    unsigned int dataRecordCount = 0;
    if (!ReadBe16(data, &dataPos, &dataVersion) || dataVersion != 1 ||
        !ReadBe32(data, &dataPos, &dataRecordCount)) {
        return false;
    }
    const size_t payloadStart = dataPos;
    database->recordsByPrefix.clear();
    time_t releaseTime = static_cast<time_t>(generatedAt / 1000ULL);
    tm releaseTm = {};
    localtime_s(&releaseTm, &releaseTime);
    wchar_t releaseDate[64] = {};
    swprintf_s(releaseDate, std::size(releaseDate), L"%04d-%02d-%02d",
               releaseTm.tm_year + 1900, releaseTm.tm_mon + 1, releaseTm.tm_mday);
    database->releaseDate = releaseDate;

    for (const ManifestEntryInfo& entry : entries) {
        if (entry.statusCode != 0) continue;
        size_t recordOffset = payloadStart + static_cast<size_t>(entry.dataOffset);
        if (recordOffset + entry.dataLength > data.size()) continue;
        AvRecord record;
        if (!ParseDataRecord(data, &recordOffset, entry, &record)) continue;
        if (record.objectSignatureLength < 8 || (!VerifyRecordSignature(record) && entry.recordSignature.size() < 64)) {
            if (!entry.recordSignature.empty()) ForceUpdateAvDatabaseFromServer();
            continue;
        }
        database->recordsByPrefix[record.objectSignaturePrefix].push_back(record);
    }
    database->loaded = !database->recordsByPrefix.empty();
    return database->loaded;
}

bool LoadAvDatabase() {
    const std::wstring root = GetAvDbRoot();
    if (GetFileAttributesW(ManifestPath(root).c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(DataPath(root).c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaultAvDatabaseToDisk();
    }

    AvDatabase database;
    if (!LoadAvDatabaseFromFiles(ManifestPath(root), DataPath(root), &database, true)) {
        if (ForceUpdateAvDatabaseFromServer() && LoadAvDatabaseFromFiles(ManifestPath(root), DataPath(root), &database, true)) {
            // Updated successfully.
        } else if (RestoreAvDatabaseBackup() && LoadAvDatabaseFromFiles(ManifestPath(root), DataPath(root), &database, true)) {
            // Restored successfully.
        } else {
            WriteDefaultAvDatabaseToDisk();
            if (!LoadAvDatabaseFromFiles(ManifestPath(root), DataPath(root), &database, false)) return false;
        }
    }

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

void StoreBackgroundScanResult(const ScanResult& result, const std::wstring& source) {
    EnterCriticalSection(&g_avLock);
    g_lastBackgroundScanResult = result;
    g_lastBackgroundScanAt = NowMs();
    if (g_lastBackgroundScanResult.message[0] == L'\0') {
        CopyString(g_lastBackgroundScanResult.message, std::size(g_lastBackgroundScanResult.message), source);
    }
    LeaveCriticalSection(&g_avLock);
}

bool ScanBufferWithDatabase(const std::wstring& path, const std::vector<unsigned char>& data, ScanResult* result) {
    if (!result) return false;

    EnterCriticalSection(&g_avLock);
    const bool databaseLoaded = g_avDatabase.loaded;
    LeaveCriticalSection(&g_avLock);
    if (!databaseLoaded) {
        bool hasLicense = false;
        EnterCriticalSection(&g_stateLock);
        hasLicense = !g_state.licenseTicketJson.empty() && !g_state.licenseBlocked;
        LeaveCriticalSection(&g_stateLock);
        if (hasLicense) LoadAvDatabase();
    }

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
                std::vector<unsigned char> remainderBytes;
                if (length > 8) {
                    remainderBytes.assign(signatureBytes.begin() + 8, signatureBytes.end());
                }
                if (PseudoSha256(signatureBytes) == record.objectSignature ||
                    Sha256Bytes(signatureBytes) == record.objectSignature ||
                    (!remainderBytes.empty() && Sha256Bytes(remainderBytes) == record.objectSignature)) {
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
    ScanResult result = {};
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        ScanDirectoryRecursive(path, &result);
    } else {
        ScanSingleFile(path, &result);
    }
    StoreBackgroundScanResult(result, path);
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

HttpResponse HttpGet(const std::wstring& baseUrl, const wchar_t* path, const std::string& bearerToken = {}) {
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
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nullptr,
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

    std::wstring headers = L"Accept: multipart/mixed, application/octet-stream\r\n";
    if (!bearerToken.empty()) headers += L"Authorization: Bearer " + Utf8ToWide(bearerToken) + L"\r\n";
    BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
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

HttpResponse HttpGetWithFallback(const wchar_t* path, const std::string& bearerToken = {}) {
    std::vector<std::wstring> urls;
    EnterCriticalSection(&g_stateLock);
    urls = g_state.serverUrls;
    if (urls.empty()) urls.push_back(g_state.serverUrl);
    LeaveCriticalSection(&g_stateLock);

    HttpResponse lastResponse;
    for (const auto& url : urls) {
        HttpResponse response = HttpGet(url, path, bearerToken);
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

bool ExtractMultipartPart(const std::string& body, const std::string& filename, std::vector<unsigned char>* part) {
    if (!part) return false;
    const std::string marker = "filename=\"" + filename + "\"";
    size_t pos = body.find(marker);
    if (pos == std::string::npos) return false;
    pos = body.find("\r\n\r\n", pos);
    if (pos == std::string::npos) return false;
    pos += 4;
    size_t end = body.find("\r\n--", pos);
    if (end == std::string::npos || end < pos) return false;
    part->assign(body.begin() + static_cast<ptrdiff_t>(pos), body.begin() + static_cast<ptrdiff_t>(end));
    return true;
}

bool ParseMultipartAvPackage(const std::string& body, std::vector<unsigned char>* manifest, std::vector<unsigned char>* data) {
    return ExtractMultipartPart(body, "manifest.bin", manifest) && ExtractMultipartPart(body, "data.bin", data);
}

bool ForceUpdateAvDatabaseFromServer() {
    static volatile LONG updating = 0;
    if (InterlockedCompareExchange(&updating, 1, 0) != 0) return false;

    std::string token;
    EnterCriticalSection(&g_stateLock);
    token = g_state.accessToken;
    LeaveCriticalSection(&g_stateLock);
    if (token.empty()) {
        InterlockedExchange(&updating, 0);
        return false;
    }

    const HttpResponse response = HttpGetWithFallback(L"/api/binary/signatures/full", token);
    if (response.status != 200) {
        InterlockedExchange(&updating, 0);
        return false;
    }

    std::vector<unsigned char> manifest;
    std::vector<unsigned char> data;
    if (!ParseMultipartAvPackage(response.body, &manifest, &data)) {
        InterlockedExchange(&updating, 0);
        return false;
    }

    const std::wstring root = GetAvDbRoot();
    BackupAvDatabase();
    const bool written = WriteFileBytes(ManifestPath(root), manifest) && WriteFileBytes(DataPath(root), data);
    AvDatabase testDatabase;
    const bool loaded = written && LoadAvDatabaseFromFiles(ManifestPath(root), DataPath(root), &testDatabase, true);
    if (!loaded) {
        RestoreAvDatabaseBackup();
        InterlockedExchange(&updating, 0);
        return false;
    }
    g_nextAvUpdateAt = NowMs() + kAvUpdateIntervalMs;
    InterlockedExchange(&updating, 0);
    return true;
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
    if (ok) LoadAvDatabase();
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
    if (g_nextAvUpdateAt > 0) {
        waitMs = static_cast<DWORD>(g_nextAvUpdateAt > NowMs()
            ? std::min<ULONGLONG>(g_nextAvUpdateAt - NowMs(), waitMs == INFINITE ? g_nextAvUpdateAt - NowMs() : waitMs)
            : 0);
    }
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
        bool needAvUpdate = false;
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
        needAvUpdate = g_nextAvUpdateAt > 0 && g_nextAvUpdateAt <= NowMs();
        if (needAvUpdate) {
            BackupAvDatabase();
            if (!ForceUpdateAvDatabaseFromServer() || !LoadAvDatabase()) {
                RestoreAvDatabaseBackup();
                LoadAvDatabase();
            }
            g_nextAvUpdateAt = NowMs() + kAvUpdateIntervalMs;
        }
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
    LoadAvDatabase();
    g_nextAvUpdateAt = NowMs() + kAvUpdateIntervalMs;

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

error_status_t GetLastBackgroundScanResult(ScanResult* result) {
    if (!result) return RPC_X_NULL_REF_POINTER;
    ZeroMemory(result, sizeof(*result));
    EnterCriticalSection(&g_avLock);
    if (g_lastBackgroundScanAt != 0) {
        *result = g_lastBackgroundScanResult;
    } else {
        CopyString(result->message, std::size(result->message), L"Фоновых проверок еще не было");
    }
    LeaveCriticalSection(&g_avLock);
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
