#include "launcher_auth.h"
#include "launcher_resource.h"

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <aclapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace AxiomAuth {
namespace {

constexpr wchar_t kDefaultApiBase[] =
    L"https://vljgmubfztmxsyiwrity.supabase.co/functions/v1/axiom-license";
constexpr size_t kMaximumResponseBytes = 32u * 1024u * 1024u;
constexpr std::array<unsigned char, 64> kServerPublicKey{
    0xc2,0x77,0xec,0x30,0xa5,0x9c,0x5e,0x87,
    0x7d,0x82,0x9c,0x2c,0x79,0x6d,0xfe,0x9a,
    0x27,0x1d,0xdb,0x3e,0x2d,0x2f,0x54,0x33,
    0xc1,0x99,0x4c,0x35,0x04,0xe0,0x34,0x44,
    0x4d,0xca,0x71,0x3f,0x25,0x61,0xbd,0x1c,
    0x80,0xf8,0x36,0xbb,0xf9,0x08,0xfb,0xd4,
    0xd3,0x12,0xda,0x4b,0xcd,0xe2,0x6a,0xb2,
    0x53,0xc2,0x42,0xb9,0xf4,0x24,0x4f,0x1d
};

struct ScopedInternet {
    HINTERNET value{};
    ScopedInternet() = default;
    explicit ScopedInternet(HINTERNET handle) : value(handle) {}
    ~ScopedInternet() { if (value) WinHttpCloseHandle(value); }
    ScopedInternet(const ScopedInternet&) = delete;
    ScopedInternet& operator=(const ScopedInternet&) = delete;
    operator HINTERNET() const { return value; }
};

bool RestrictPathToCurrentUser(const std::filesystem::path& path,
                               bool inheritToChildren) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        return false;
    const auto closeToken = std::unique_ptr<void, decltype([](void* handle) {
        if (handle) CloseHandle(static_cast<HANDLE>(handle));
    })>(rawToken, {});
    DWORD bytes = 0;
    GetTokenInformation(rawToken, TokenUser, nullptr, 0, &bytes);
    if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<unsigned char> tokenBytes(bytes);
    if (!GetTokenInformation(rawToken, TokenUser, tokenBytes.data(), bytes,
                             &bytes)) return false;
    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBytes.data());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = inheritToChildren
        ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
        : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(tokenUser->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS)
        return false;
    const auto freeAcl = std::unique_ptr<void, decltype([](void* value) {
        if (value) LocalFree(value);
    })>(acl, {});
    std::wstring mutablePath = path.wstring();
    return SetNamedSecurityInfoW(
        mutablePath.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
}

std::filesystem::path LauncherDataDirectory() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                    nullptr, &localAppData)))
        return std::filesystem::temp_directory_path() / L"Axiom" / L"launcher";
    std::filesystem::path result =
        std::filesystem::path(localAppData) / L"Axiom" / L"launcher";
    CoTaskMemFree(localAppData);
    std::error_code ignored;
    std::filesystem::create_directories(result, ignored);
    if (!ignored) RestrictPathToCurrentUser(result, true);
    return result;
}

#ifdef AXIOM_OFFLINE_TEST_MODE
bool AcquireEmbeddedTestModule(std::filesystem::path& modulePath,
                               std::wstring& error) {
    const HMODULE instance = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        instance, MAKEINTRESOURCEW(IDR_AXIOM_MODULE), RT_RCDATA);
    if (!resource) {
        error = L"В лаунчере отсутствует тестовый модуль.";
        return false;
    }
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const auto* bytes = loaded
        ? static_cast<const unsigned char*>(LockResource(loaded))
        : nullptr;
    if (!bytes || size < 4096 || bytes[0] != 'M' || bytes[1] != 'Z') {
        error = L"Встроенный тестовый модуль повреждён.";
        return false;
    }

    modulePath = LauncherDataDirectory() /
        (L"Axiom-local-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L".dll");
    std::ofstream output(modulePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = L"Не удалось подготовить встроенный тестовый модуль.";
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes), size);
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(modulePath, ignored);
        modulePath.clear();
        error = L"Не удалось сохранить встроенный тестовый модуль.";
        return false;
    }
    return true;
}
#endif

std::wstring Trim(std::wstring value) {
    const auto whitespace = [](wchar_t character) { return iswspace(character) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    return value;
}

std::wstring NormalizeLicense(std::wstring value) {
    value = Trim(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), towupper);
    return value;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr))
        return {};
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (characters <= 0) return {};
    std::wstring result(static_cast<size_t>(characters), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), characters))
        return {};
    return result;
}

bool Sha256(const void* data, size_t size, std::array<unsigned char, 32>& digest) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectSize = 0, hashSize = 0, received = 0;
    std::vector<unsigned char> object;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) return false;
    const auto closeAlgorithm = std::unique_ptr<void, decltype([](void* handle) {
        if (handle) BCryptCloseAlgorithmProvider(
            static_cast<BCRYPT_ALG_HANDLE>(handle), 0);
    })>(algorithm, {});
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &received, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &received, 0) < 0 ||
        hashSize != digest.size()) return false;
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
                         nullptr, 0, 0) < 0) return false;
    const auto closeHash = std::unique_ptr<void, decltype([](void* handle) {
        if (handle) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle));
    })>(hash, {});
    if (size && BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(size), 0) < 0) return false;
    return BCryptFinishHash(hash, digest.data(),
                            static_cast<ULONG>(digest.size()), 0) >= 0;
}

std::string Hex(const unsigned char* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        result[i * 2] = digits[data[i] >> 4];
        result[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return result;
}

bool DecodeBase64Url(std::string value, std::vector<unsigned char>& output) {
    for (char& character : value) {
        if (character == '-') character = '+';
        else if (character == '_') character = '/';
        else if (!std::isalnum(static_cast<unsigned char>(character)) &&
                 character != '+' && character != '/') return false;
    }
    value.append((4 - value.size() % 4) % 4, '=');
    DWORD bytes = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
            CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr)) return false;
    output.resize(bytes);
    return CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
        CRYPT_STRING_BASE64, output.data(), &bytes, nullptr, nullptr) != FALSE;
}

std::string EscapeJson(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20) return {};
                result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

std::string JsonString(const std::string& json, std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\":\"";
    const size_t start = json.find(marker);
    if (start == std::string::npos) return {};
    size_t cursor = start + marker.size();
    std::string result;
    while (cursor < json.size()) {
        const char character = json[cursor++];
        if (character == '"') return result;
        if (character == '\\') {
            if (cursor >= json.size()) return {};
            const char escaped = json[cursor++];
            if (escaped == '"' || escaped == '\\' || escaped == '/')
                result.push_back(escaped);
            else return {};
        } else {
            result.push_back(character);
        }
    }
    return {};
}

std::wstring LoadApiBase() {
    // The production endpoint is pinned in the signed executable. A writable
    // local override would only add a denial-of-service/redirection surface.
    return kDefaultApiBase;
}

bool ParseApiBase(const std::wstring& base, std::wstring& host,
                  INTERNET_PORT& port, bool& secure, std::wstring& basePath,
                  std::wstring& extraInfo) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(base.c_str(), static_cast<DWORD>(base.size()), 0,
                         &components)) return false;
    host.assign(components.lpszHostName, components.dwHostNameLength);
    basePath.assign(components.lpszUrlPath, components.dwUrlPathLength);
    extraInfo = components.dwExtraInfoLength
        ? std::wstring(components.lpszExtraInfo, components.dwExtraInfoLength)
        : std::wstring{};
    port = components.nPort;
    secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    const bool loopback = _wcsicmp(host.c_str(), L"127.0.0.1") == 0 ||
                          _wcsicmp(host.c_str(), L"localhost") == 0 ||
                          _wcsicmp(host.c_str(), L"::1") == 0;
    return secure || loopback;
}

bool HttpRequest(const wchar_t* method, const std::wstring& endpoint,
                 const std::string& body, const std::wstring& extraHeaders,
                 std::vector<unsigned char>& response, DWORD& status,
                 std::wstring& error) {
    const bool absoluteUrl = endpoint.starts_with(L"https://") ||
                             endpoint.starts_with(L"http://");
    const std::wstring requestBase = absoluteUrl ? endpoint : LoadApiBase();
    std::wstring host, basePath, extraInfo;
    INTERNET_PORT port = 0;
    bool secure = false;
    if (!ParseApiBase(requestBase, host, port, secure, basePath, extraInfo)) {
        error = L"Некорректный адрес сервера лицензий.";
        return false;
    }
    if (!absoluteUrl)
        while (!basePath.empty() && basePath.back() == L'/') basePath.pop_back();
    std::wstring path = absoluteUrl ? basePath + extraInfo : basePath + endpoint;
    ScopedInternet session(WinHttpOpen(L"AxiomLauncher/1.0.1",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) {
        error = L"Не удалось открыть сетевое соединение.";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    ScopedInternet connection(WinHttpConnect(session, host.c_str(), port, 0));
    if (!connection.value) {
        error = L"Сервер лицензий недоступен.";
        return false;
    }
    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    ScopedInternet request(WinHttpOpenRequest(connection, method, path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.value) {
        error = L"Не удалось создать запрос к серверу.";
        return false;
    }
    const wchar_t* headers = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                   : extraHeaders.c_str();
    const DWORD headerLength = extraHeaders.empty() ? 0
        : static_cast<DWORD>(extraHeaders.size());
    void* requestBody = body.empty() ? WINHTTP_NO_REQUEST_DATA
                                     : const_cast<char*>(body.data());
    if (!WinHttpSendRequest(request, headers, headerLength, requestBody,
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = L"Сервер лицензий не отвечает.";
        return false;
    }
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        error = L"Некорректный ответ сервера лицензий.";
        return false;
    }
    response.clear();
    std::array<unsigned char, 16384> chunk{};
    for (;;) {
        DWORD received = 0;
        if (!WinHttpReadData(request, chunk.data(),
                             static_cast<DWORD>(chunk.size()), &received)) {
            error = L"Ошибка загрузки данных с сервера.";
            return false;
        }
        if (!received) break;
        if (response.size() + received > kMaximumResponseBytes) {
            error = L"Ответ сервера слишком большой.";
            return false;
        }
        response.insert(response.end(), chunk.begin(), chunk.begin() + received);
    }
    return true;
}

std::string DeviceHash() {
    wchar_t machineGuid[256]{};
    DWORD guidBytes = sizeof(machineGuid);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                 L"MachineGuid", RRF_RT_REG_SZ, nullptr, machineGuid, &guidBytes);
    wchar_t windowsDirectory[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    wchar_t root[] = L"C:\\";
    if (windowsDirectory[1] == L':') root[0] = windowsDirectory[0];
    DWORD volumeSerial = 0;
    GetVolumeInformationW(root, nullptr, 0, &volumeSerial, nullptr, nullptr,
                          nullptr, 0);
    const std::wstring material = std::wstring(machineGuid) + L"|" +
                                  std::to_wstring(volumeSerial) + L"|Axiom-v1";
    const std::string bytes = WideToUtf8(material);
    std::array<unsigned char, 32> digest{};
    if (bytes.empty() || !Sha256(bytes.data(), bytes.size(), digest)) return {};
    return Hex(digest.data(), digest.size());
}

std::string RandomNonce() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    return Hex(bytes.data(), bytes.size());
}

bool VerifySignedToken(const std::string& token, const std::string& device,
                       const std::string& nonce, std::string& expectedHash,
                       size_t& expectedSize) {
    const size_t separator = token.find('.');
    if (separator == std::string::npos || token.find('.', separator + 1) != std::string::npos)
        return false;
    std::vector<unsigned char> payload, signature;
    if (!DecodeBase64Url(token.substr(0, separator), payload) ||
        !DecodeBase64Url(token.substr(separator + 1), signature) ||
        signature.size() != 64) return false;
    std::array<unsigned char, 32> digest{};
    if (!Sha256(payload.data(), payload.size(), digest)) return false;

    struct EccHeader { ULONG magic; ULONG keyBytes; };
    std::array<unsigned char, sizeof(EccHeader) + kServerPublicKey.size()> blob{};
    const EccHeader header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), kServerPublicKey.data(),
                kServerPublicKey.size());
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_KEY_HANDLE key{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) < 0) return false;
    const auto closeAlgorithm = std::unique_ptr<void, decltype([](void* handle) {
        if (handle) BCryptCloseAlgorithmProvider(
            static_cast<BCRYPT_ALG_HANDLE>(handle), 0);
    })>(algorithm, {});
    if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
            blob.data(), static_cast<ULONG>(blob.size()), 0) < 0) return false;
    const auto closeKey = std::unique_ptr<void, decltype([](void* handle) {
        if (handle) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(handle));
    })>(key, {});
    if (BCryptVerifySignature(key, nullptr, digest.data(),
            static_cast<ULONG>(digest.size()), signature.data(),
            static_cast<ULONG>(signature.size()), 0) < 0) return false;

    const std::string payloadText(payload.begin(), payload.end());
    std::map<std::string, std::string> fields;
    size_t start = 0;
    while (start <= payloadText.size()) {
        const size_t end = payloadText.find('&', start);
        const std::string pair = payloadText.substr(start, end - start);
        const size_t equals = pair.find('=');
        if (equals == std::string::npos || equals == 0) return false;
        fields.emplace(pair.substr(0, equals), pair.substr(equals + 1));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    try {
        const long long expiration = std::stoll(fields.at("expires"));
        const long long now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const unsigned long long declaredSize = std::stoull(fields.at("size"));
        if (fields.at("v") != "1" || fields.at("device") != device ||
            fields.at("nonce") != nonce || expiration <= now ||
            expiration > now + 600 ||
            declaredSize < 4096 || declaredSize > kMaximumResponseBytes ||
            fields.at("sha256").size() != 64) return false;
        expectedHash = fields.at("sha256");
        expectedSize = static_cast<size_t>(declaredSize);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool ProtectCredential(const std::wstring& license) {
    const std::wstring entropyText = L"AxiomLauncher/Credential/v1";
    DATA_BLOB input{static_cast<DWORD>(license.size() * sizeof(wchar_t)),
                    reinterpret_cast<BYTE*>(const_cast<wchar_t*>(license.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(entropyText.size() * sizeof(wchar_t)),
                      reinterpret_cast<BYTE*>(const_cast<wchar_t*>(entropyText.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Axiom license", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    std::ofstream file(LauncherDataDirectory() / L"credential.bin",
                       std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    const bool success = file.good() &&
                         RestrictPathToCurrentUser(
                             LauncherDataDirectory() / L"credential.bin", false);
    LocalFree(output.pbData);
    return success;
}

void CleanupStaleModules() {
    const std::filesystem::path directory = LauncherDataDirectory();
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error) continue;
        const std::wstring name = iterator->path().filename().wstring();
        if (name.starts_with(L"Axiom-") &&
            iterator->path().extension() == L".dll") {
            std::filesystem::remove(iterator->path(), error);
            error.clear();
        }
    }
}

}  // namespace

std::wstring LoadSavedLicense() {
    std::ifstream file(LauncherDataDirectory() / L"credential.bin", std::ios::binary);
    std::vector<unsigned char> protectedBytes((std::istreambuf_iterator<char>(file)), {});
    if (protectedBytes.empty() || protectedBytes.size() > 16384) return {};
    const std::wstring entropyText = L"AxiomLauncher/Credential/v1";
    DATA_BLOB input{static_cast<DWORD>(protectedBytes.size()), protectedBytes.data()};
    DATA_BLOB entropy{static_cast<DWORD>(entropyText.size() * sizeof(wchar_t)),
                      reinterpret_cast<BYTE*>(const_cast<wchar_t*>(entropyText.data()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    std::wstring result;
    if (output.cbData % sizeof(wchar_t) == 0) {
        result.assign(reinterpret_cast<wchar_t*>(output.pbData),
                      output.cbData / sizeof(wchar_t));
    }
    LocalFree(output.pbData);
    return result;
}

bool AuthenticateAndAcquireModule(const std::wstring& rawLicense,
                                  std::filesystem::path& modulePath,
                                  std::wstring& error) {
#ifdef AXIOM_OFFLINE_TEST_MODE
    (void)rawLicense;
    CleanupStaleModules();
    return AcquireEmbeddedTestModule(modulePath, error);
#else
    const std::wstring license = NormalizeLicense(rawLicense);
    if (license.size() < 12) {
        error = L"Введите лицензионный ключ.";
        return false;
    }
    CleanupStaleModules();
    const std::string device = DeviceHash();
    const std::string nonce = RandomNonce();
    if (device.empty() || nonce.empty()) {
        error = L"Не удалось определить устройство.";
        return false;
    }
    const std::string utf8License = EscapeJson(WideToUtf8(license));
    if (utf8License.empty()) {
        error = L"Некорректный лицензионный ключ.";
        return false;
    }
    const std::string body = "{\"key\":\"" + utf8License +
        "\",\"device\":\"" + device + "\",\"nonce\":\"" + nonce +
        "\",\"version\":\"" + WideToUtf8(kLauncherVersion) + "\"}";
    std::vector<unsigned char> response;
    DWORD status = 0;
    if (!HttpRequest(L"POST", L"/v1/session", body,
            L"Content-Type: application/json\r\n", response, status, error))
        return false;
    if (status != HTTP_STATUS_OK) {
        if (status == 401)
            error = L"Неверный или недействительный лицензионный ключ.";
        else if (status == 403)
            error = L"Лицензия заблокирована или уже привязана к другому ПК.";
        else if (status == 429)
            error = L"Слишком много попыток. Попробуйте через минуту.";
        else
            error = L"Сервер лицензий временно недоступен.";
        return false;
    }
    const std::string responseText(response.begin(), response.end());
    const std::string token = JsonString(responseText, "token");
    const std::string moduleUrl = JsonString(responseText, "module_url");
    std::string expectedHash;
    size_t expectedSize = 0;
    if (token.empty() || !VerifySignedToken(token, device, nonce,
                                            expectedHash, expectedSize)) {
        error = L"Подпись ответа сервера недействительна.";
        return false;
    }
    modulePath = LauncherDataDirectory() /
        (L"Axiom-" + Utf8ToWide(expectedHash.substr(0, 16)) + L"-" +
         Utf8ToWide(nonce.substr(0, 8)) + L".dll");
    std::vector<unsigned char> module;
    module.reserve(expectedSize);
    if (!moduleUrl.empty()) {
        const std::wstring downloadUrl = Utf8ToWide(moduleUrl);
        if (downloadUrl.empty() || !HttpRequest(L"GET", downloadUrl, {}, {},
                                                module, status, error)) return false;
    } else {
        // Compatibility with the original self-hosted service.
        const std::wstring authorization = L"Authorization: Bearer " +
                                           Utf8ToWide(token) + L"\r\n";
        constexpr size_t kModuleChunkSize = 512u * 1024u;
        while (module.size() < expectedSize) {
            const size_t remaining = expectedSize - module.size();
            const size_t requested = (std::min)(remaining, kModuleChunkSize);
            const std::wstring endpoint = L"/v1/module?offset=" +
                std::to_wstring(module.size()) + L"&length=" +
                std::to_wstring(requested);
            std::vector<unsigned char> chunk;
            if (!HttpRequest(L"GET", endpoint, {}, authorization,
                             chunk, status, error)) return false;
            if (status != HTTP_STATUS_OK || chunk.empty() ||
                chunk.size() > requested || module.size() + chunk.size() > expectedSize) {
                error = L"Получен некорректный фрагмент модуля.";
                return false;
            }
            module.insert(module.end(), chunk.begin(), chunk.end());
        }
    }
    std::array<unsigned char, 32> digest{};
    if (status != HTTP_STATUS_OK || module.size() != expectedSize ||
        module.size() < 4096 || module[0] != 'M' || module[1] != 'Z' ||
        !Sha256(module.data(), module.size(), digest) ||
        Hex(digest.data(), digest.size()) != expectedHash) {
        error = L"Полученный модуль повреждён или подменён.";
        return false;
    }
    const std::filesystem::path temporary = modulePath.wstring() + L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(module.data()), module.size());
    file.close();
    if (!module.empty()) SecureZeroMemory(module.data(), module.size());
    if (!file) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = L"Не удалось полностью записать защищённый модуль.";
        return false;
    }
    if (!RestrictPathToCurrentUser(temporary, false)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = L"Не удалось защитить временный модуль от других пользователей.";
        return false;
    }
    std::error_code renameError;
    std::filesystem::rename(temporary, modulePath, renameError);
    if (renameError) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = L"Не удалось сохранить защищённый модуль.";
        return false;
    }
    ProtectCredential(license);
    return true;
#endif
}

}  // namespace AxiomAuth
