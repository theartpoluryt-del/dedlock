#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "launcher_resource.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"AxiomLauncherWindow";
constexpr wchar_t kWindowTitle[] = L"Axiom Launcher";
constexpr wchar_t kTargetProcess[] = L"deadlock.exe";
constexpr wchar_t kSteamLaunchUri[] = L"steam://rungameid/1422450";
constexpr wchar_t kDefaultLocalKey[] = L"ARTPO-LOCAL-2026";
constexpr int kChromeHeight = 38;
constexpr int kContentLift = 28;
constexpr UINT kLaunchFinished = WM_APP + 1;

// SHA-256 of the normalized local key. Replace this validator with the server
// implementation later; the UI and launch pipeline do not depend on it.
constexpr std::array<std::wstring_view, 1> kLocalKeyHashes{
    L"77b4f5801fa4d5c3041c198039e797fa0e989f45b8d8e35c46e4c33d1e761c5b"
};

HWND g_keyEdit{};
HWND g_launchButton{};
HFONT g_bodyFont{};
HBRUSH g_backgroundBrush{};
HBRUSH g_editBrush{};
ULONG_PTR g_gdiplusToken{};
IStream* g_logoStream{};
IStream* g_backgroundStream{};
std::unique_ptr<Gdiplus::Bitmap> g_logoBitmap;
std::unique_ptr<Gdiplus::Bitmap> g_backgroundBitmap;
WNDPROC g_keyEditProc{};
int g_chromeHover{};
int g_chromePressed{};
std::wstring g_statusText{L"Готов к запуску"};
COLORREF g_statusColor{RGB(152, 158, 172)};
bool g_launching{};

struct ScopedHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : value(handle) {}
    ~ScopedHandle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    operator HANDLE() const { return value; }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::wstring TrimAndNormalize(std::wstring value) {
    const auto whitespace = [](wchar_t character) {
        return iswspace(character) != 0;
    };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), towupper);
    return value;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

bool Sha256(const std::string& value, std::array<unsigned char, 32>& digest) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD received = 0;
    std::vector<unsigned char> object;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) return false;
    const auto closeAlgorithm = std::unique_ptr<void, decltype([](void* handle) {
        if (handle) BCryptCloseAlgorithmProvider(
            static_cast<BCRYPT_ALG_HANDLE>(handle), 0);
    })>(algorithm, {});
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize),
                          sizeof(objectSize), &received, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashSize),
                          sizeof(hashSize), &received, 0) < 0 ||
        hashSize != digest.size()) return false;
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
                         nullptr, 0, 0) < 0) return false;
    const auto closeHash = std::unique_ptr<void, decltype([](void* handle) {
        if (handle) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle));
    })>(hash, {});
    if (BCryptHashData(hash,
                       reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                       static_cast<ULONG>(value.size()), 0) < 0) return false;
    return BCryptFinishHash(hash, digest.data(),
                            static_cast<ULONG>(digest.size()), 0) >= 0;
}

std::wstring HexDigest(const std::array<unsigned char, 32>& digest) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.resize(digest.size() * 2);
    for (size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = digits[digest[i] >> 4];
        result[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    return result;
}

bool ValidateLocalLicense(const std::wstring& rawKey) {
    const std::wstring normalized = TrimAndNormalize(rawKey);
    if (normalized.empty()) return false;
    std::array<unsigned char, 32> digest{};
    if (!Sha256(WideToUtf8(normalized), digest)) return false;
    const std::wstring encoded = HexDigest(digest);
    return std::find(kLocalKeyHashes.begin(), kLocalKeyHashes.end(), encoded) !=
           kLocalKeyHashes.end();
}

DWORD FindProcessId(std::wstring_view executable) {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) return 0;
    do {
        if (_wcsicmp(entry.szExeFile, executable.data()) == 0)
            return entry.th32ProcessID;
    } while (Process32NextW(snapshot, &entry));
    return 0;
}

uintptr_t FindRemoteModule(DWORD processId, std::wstring_view moduleName) {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId));
    if (!snapshot.valid()) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot, &entry)) return 0;
    do {
        if (_wcsicmp(entry.szModule, moduleName.data()) == 0)
            return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snapshot, &entry));
    return 0;
}

bool HasVisibleGameWindow(DWORD processId) {
    struct SearchContext {
        DWORD processId;
        bool found;
    } context{processId, false};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* search = reinterpret_cast<SearchContext*>(parameter);
        DWORD ownerProcess = 0;
        GetWindowThreadProcessId(window, &ownerProcess);
        if (ownerProcess == search->processId && IsWindowVisible(window) &&
            GetWindow(window, GW_OWNER) == nullptr) {
            RECT bounds{};
            if (GetClientRect(window, &bounds) && bounds.right > 640 &&
                bounds.bottom > 360) {
                search->found = true;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

bool IsGameRuntimeReady(DWORD processId) {
    constexpr std::array<std::wstring_view, 6> modules{
        L"client.dll", L"engine2.dll", L"schemasystem.dll", L"tier0.dll",
        L"rendersystemdx11.dll", L"d3d11.dll"};
    if (!HasVisibleGameWindow(processId)) return false;
    return std::all_of(modules.begin(), modules.end(), [processId](auto module) {
        return FindRemoteModule(processId, module) != 0;
    });
}

bool IsPayloadLoaded(DWORD processId) {
    const std::wstring guardName =
        L"Local\\Dll6_Deadlock_SingleInstance_" +
        std::to_wstring(processId);
    HANDLE guard = OpenMutexW(SYNCHRONIZE, FALSE, guardName.c_str());
    if (guard) {
        CloseHandle(guard);
        return true;
    }
    // Keep the module check as a fallback during the short interval between
    // LoadLibrary returning and the DLL initialization thread creating guard.
    return FindRemoteModule(processId, L"Dll6.dll") != 0;
}

bool IsPayloadReady(DWORD processId) {
    const std::wstring eventName = L"Local\\Dll6_Deadlock_Ready_" +
                                   std::to_wstring(processId);
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
    if (!event) return false;
    const bool ready = WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    CloseHandle(event);
    return ready;
}

HWND FindGameWindow(DWORD processId) {
    struct SearchContext {
        DWORD processId;
        HWND window;
    } context{processId, nullptr};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* search = reinterpret_cast<SearchContext*>(parameter);
        DWORD ownerProcess = 0;
        GetWindowThreadProcessId(window, &ownerProcess);
        if (ownerProcess == search->processId && IsWindowVisible(window) &&
            GetWindow(window, GW_OWNER) == nullptr) {
            RECT bounds{};
            if (GetClientRect(window, &bounds) && bounds.right > 640 &&
                bounds.bottom > 360) {
                search->window = window;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

bool UnloadPayload(DWORD processId, std::wstring& error) {
    if (!IsPayloadLoaded(processId)) return true;
    const HWND window = FindGameWindow(processId);
    if (!window) {
        error = L"Не удалось найти окно Deadlock для выгрузки DLL.";
        return false;
    }
    // Dll6 handles this private launcher message by entering its orderly
    // shutdown path: hooks and
    // worker threads are removed before FreeLibraryAndExitThread releases
    // the module. Use the same path instead of calling remote FreeLibrary,
    // which would leave MinHook callbacks pointing into an unloaded image.
    PostMessageW(window, WM_APP + 0x6D6, 0, 0);
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        if (!IsPayloadLoaded(processId)) return true;
        Sleep(50);
    }
    error = L"Предыдущая DLL не успела корректно выгрузиться.";
    return false;
}

bool StartDeadlock(std::wstring& error) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", kSteamLaunchUri, nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32) return true;
    error = L"Не удалось запустить Deadlock через Steam.";
    return false;
}

DWORD WaitForDeadlock(std::wstring& error) {
    DWORD processId = FindProcessId(kTargetProcess);
    if (!processId && !StartDeadlock(error)) return 0;

    const ULONGLONG deadline = GetTickCount64() + 180000;
    ULONGLONG readySince = 0;
    while (GetTickCount64() < deadline) {
        processId = FindProcessId(kTargetProcess);
        if (processId && IsGameRuntimeReady(processId)) {
            if (!readySince) readySince = GetTickCount64();
            if (GetTickCount64() - readySince >= 2500) return processId;
        } else {
            readySince = 0;
        }
        Sleep(250);
    }
    error = L"Deadlock не успел полностью загрузиться за 180 секунд.";
    return 0;
}

bool ExtractEmbeddedDll(HINSTANCE instance, std::filesystem::path& output,
                        std::wstring& error) {
    const HRSRC resource = FindResourceW(
        instance, MAKEINTRESOURCEW(IDR_PAYLOAD_DLL), RT_RCDATA);
    if (!resource) {
        error = L"В EXE отсутствует встроенная DLL.";
        return false;
    }
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size < 0x1000) {
        error = L"Встроенная DLL повреждена.";
        return false;
    }
    std::array<unsigned char, 32> payloadDigest{};
    const std::string payloadBytes(static_cast<const char*>(data), size);
    if (!Sha256(payloadBytes, payloadDigest)) {
        error = L"Не удалось проверить встроенную DLL.";
        return false;
    }

    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                    nullptr, &localAppData))) {
        error = L"Не удалось открыть LOCALAPPDATA.";
        return false;
    }
    const std::wstring payloadId = HexDigest(payloadDigest).substr(0, 16);
    output = std::filesystem::path(localAppData) / L"Axiom" / L"launcher" /
             (L"Axiom-" + payloadId + L".dll");
    CoTaskMemFree(localAppData);
    std::error_code directoryError;
    std::filesystem::create_directories(output.parent_path(), directoryError);
    if (directoryError) {
        error = L"Не удалось создать папку launcher.";
        return false;
    }
    std::error_code existingError;
    if (std::filesystem::exists(output, existingError) && !existingError &&
        std::filesystem::file_size(output, existingError) == size &&
        !existingError) return true;

    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"Не удалось извлечь встроенную DLL.";
        return false;
    }
    file.write(static_cast<const char*>(data), size);
    file.close();
    if (!file) {
        error = L"Ошибка записи встроенной DLL.";
        return false;
    }
    return true;
}

bool LoadPayload(DWORD processId, const std::filesystem::path& dllPath,
                 std::wstring& error) {
    if (!UnloadPayload(processId, error)) return false;
    ScopedHandle process(OpenProcess(PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId));
    if (!process.valid()) {
        error = L"Нет доступа к процессу Deadlock. Запусти launcher с теми же правами, что и игру.";
        return false;
    }

    const std::wstring path = dllPath.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        error = L"Не удалось выделить память в процессе Deadlock.";
        return false;
    }
    const auto remoteDeleter = [&](void* address) {
        if (address) VirtualFreeEx(process, address, 0, MEM_RELEASE);
    };
    const auto freeRemote = std::unique_ptr<void, decltype(remoteDeleter)>(
        remotePath, remoteDeleter);
    if (!WriteProcessMemory(process, remotePath, path.c_str(), bytes, nullptr)) {
        error = L"Не удалось передать путь DLL в процесс Deadlock.";
        return false;
    }

    const HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
    const auto localLoadLibrary = reinterpret_cast<uintptr_t>(
        GetProcAddress(localKernel, "LoadLibraryW"));
    const uintptr_t remoteKernel = FindRemoteModule(processId, L"kernel32.dll");
    if (!localKernel || !localLoadLibrary || !remoteKernel) {
        error = L"Не удалось найти LoadLibraryW в процессе Deadlock.";
        return false;
    }
    const uintptr_t loadLibraryRva = localLoadLibrary -
        reinterpret_cast<uintptr_t>(localKernel);
    const auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteKernel + loadLibraryRva);
    ScopedHandle thread(CreateRemoteThread(process, nullptr, 0,
        remoteLoadLibrary, remotePath, 0, nullptr));
    if (!thread.valid()) {
        error = L"Не удалось создать поток загрузки DLL.";
        return false;
    }
    if (WaitForSingleObject(thread, 15000) != WAIT_OBJECT_0) {
        error = L"Загрузка DLL не завершилась за 15 секунд.";
        return false;
    }
    DWORD result = 0;
    if (!GetExitCodeThread(thread, &result) || result == 0) {
        error = L"Deadlock отклонил загрузку DLL.";
        return false;
    }
    // Do not report success merely because LoadLibrary returned. The DLL
    // publishes this event only after offsets and hooks are initialized.
    const ULONGLONG readyDeadline = GetTickCount64() + 20000;
    while (GetTickCount64() < readyDeadline && !IsPayloadReady(processId))
        Sleep(25);
    if (!IsPayloadReady(processId)) {
        error = L"DLL загружена, но не завершила инициализацию.";
        return false;
    }
    error = L"Axiom успешно запущен.";
    return true;
}

void SetStatus(const wchar_t* text, COLORREF color) {
    g_statusText = text ? text : L"";
    g_statusColor = color;
    if (g_launchButton) InvalidateRect(GetParent(g_launchButton), nullptr, FALSE);
}

void BeginLaunch(HWND window) {
    if (g_launching) return;
    const int length = GetWindowTextLengthW(g_keyEdit);
    std::wstring key(static_cast<size_t>((std::max)(length, 0)), L'\0');
    if (length > 0) GetWindowTextW(g_keyEdit, key.data(), length + 1);
    if (!ValidateLocalLicense(key)) {
        SetStatus(L"Неверный лицензионный ключ", RGB(244, 91, 105));
        return;
    }

    g_launching = true;
    EnableWindow(g_launchButton, FALSE);
    SetWindowTextW(g_launchButton, L"Запуск...");
    SetStatus(L"Ожидание Deadlock...", RGB(250, 194, 78));
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(window, GWLP_HINSTANCE));
    std::thread([window, instance] {
        std::wstring message;
        std::filesystem::path payload;
        const DWORD processId = WaitForDeadlock(message);
        bool success = processId != 0;
        if (success) {
            success = ExtractEmbeddedDll(instance, payload, message);
            if (success) success = LoadPayload(processId, payload, message);
        }
        auto* result = new std::pair<bool, std::wstring>(success,
                                                         std::move(message));
        PostMessageW(window, kLaunchFinished, 0,
                     reinterpret_cast<LPARAM>(result));
    }).detach();
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect,
                    float radius) {
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter,
                270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter,
                90.0f, 90.0f);
    path.CloseFigure();
}

bool LoadEmbeddedBitmap(HINSTANCE instance, int resourceId, IStream*& stream,
                        std::unique_ptr<Gdiplus::Bitmap>& bitmap) {
    const HRSRC resource = FindResourceW(
        instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return false;
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !size) return false;

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) return false;
    void* destination = GlobalLock(copy);
    if (!destination) {
        GlobalFree(copy);
        return false;
    }
    memcpy(destination, bytes, size);
    GlobalUnlock(copy);
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream))) {
        GlobalFree(copy);
        return false;
    }
    bitmap.reset(Gdiplus::Bitmap::FromStream(stream));
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        bitmap.reset();
        stream->Release();
        stream = nullptr;
        return false;
    }
    return true;
}

void ShutdownBrandGraphics() {
    g_backgroundBitmap.reset();
    g_logoBitmap.reset();
    if (g_backgroundStream) {
        g_backgroundStream->Release();
        g_backgroundStream = nullptr;
    }
    if (g_logoStream) {
        g_logoStream->Release();
        g_logoStream = nullptr;
    }
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

void PaintLauncher(HWND window) {
    PAINTSTRUCT paint{};
    HDC targetDc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const float width = static_cast<float>(client.right);
    const float height = static_cast<float>(client.bottom);

    HDC dc = CreateCompatibleDC(targetDc);
    HBITMAP surface = CreateCompatibleBitmap(targetDc, client.right,
                                              client.bottom);
    HGDIOBJ previousSurface = SelectObject(dc, surface);

    {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush background(Gdiplus::Color(255, 10, 13, 19));
    graphics.FillRectangle(&background, 0.0f, 0.0f, width, height);
    if (g_backgroundBitmap) {
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(g_backgroundBitmap.get(),
                           Gdiplus::RectF(0, 0, width, height));
        Gdiplus::SolidBrush shade(Gdiplus::Color(72, 4, 6, 10));
        graphics.FillRectangle(&shade, 0.0f, 0.0f, width, height);
    }

    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const auto drawChromeButton = [&](int index, float left, bool closeButton) {
        const bool hovered = g_chromeHover == index;
        const bool pressed = g_chromePressed == index;
        const Gdiplus::Color symbolColor = closeButton && (hovered || pressed)
            ? Gdiplus::Color(255, 244, 46, 82)
            : hovered || pressed ? Gdiplus::Color(255, 255, 255, 255)
                                 : Gdiplus::Color(235, 187, 193, 205);
        Gdiplus::Pen symbol(symbolColor, hovered || pressed ? 1.9f : 1.6f);
        symbol.SetStartCap(Gdiplus::LineCapRound);
        symbol.SetEndCap(Gdiplus::LineCapRound);
        const float centerX = left + 20.0f;
        const float centerY = kChromeHeight * 0.5f + (pressed ? 1.0f : 0.0f);
        if (closeButton) {
            graphics.DrawLine(&symbol, centerX - 4, centerY - 4,
                              centerX + 4, centerY + 4);
            graphics.DrawLine(&symbol, centerX + 4, centerY - 4,
                              centerX - 4, centerY + 4);
        } else {
            graphics.DrawLine(&symbol, centerX - 4, centerY + 2,
                              centerX + 4, centerY + 2);
        }
    };
    drawChromeButton(1, width - 80.0f, false);
    drawChromeButton(2, width - 40.0f, true);

    Gdiplus::GraphicsPath editBorder;
    AddRoundedRect(editBorder,
                   Gdiplus::RectF(23, 126.0f + kChromeHeight - kContentLift,
                                  width - 46, 46), 7.0f);
    Gdiplus::Pen editPen(Gdiplus::Color(255, 45, 51, 63), 1.0f);
    graphics.DrawPath(&editPen, &editBorder);

    if (g_logoBitmap) {
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(g_logoBitmap.get(),
                           Gdiplus::RectF(20, 16, 70, 70));
    }

    Gdiplus::FontFamily brandFamily(L"Segoe UI Variable Display");
    Gdiplus::Font brandFont(&brandFamily, 29.0f, Gdiplus::FontStyleBold,
                            Gdiplus::UnitPixel);
    Gdiplus::FontFamily textFamily(L"Segoe UI Variable Text");
    Gdiplus::Font brandCaption(&textFamily, 13.0f,
                               Gdiplus::FontStyleBold,
                               Gdiplus::UnitPixel);
    Gdiplus::Font labelFont(&textFamily, 13.0f,
                            Gdiplus::FontStyleRegular,
                            Gdiplus::UnitPixel);
    Gdiplus::SolidBrush primary(Gdiplus::Color(255, 244, 246, 250));
    Gdiplus::SolidBrush secondary(Gdiplus::Color(255, 153, 160, 174));
    graphics.DrawString(L"AXIOM", -1, &brandFont,
                        Gdiplus::PointF(98, 25.0f), &primary);
    graphics.DrawString(L"LAUNCHER", -1, &brandCaption,
                        Gdiplus::PointF(101, 60.0f), &secondary);
    graphics.DrawString(L"Лицензионный ключ", -1, &labelFont,
                        Gdiplus::PointF(24, 99.0f + kChromeHeight - kContentLift),
                        &secondary);
    Gdiplus::SolidBrush statusBrush(Gdiplus::Color(
        255, GetRValue(g_statusColor), GetGValue(g_statusColor),
        GetBValue(g_statusColor)));
    Gdiplus::StringFormat centered;
    centered.SetAlignment(Gdiplus::StringAlignmentCenter);
    centered.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    graphics.DrawString(g_statusText.c_str(), -1, &labelFont,
                        Gdiplus::RectF(24, 248.0f + kChromeHeight - kContentLift,
                                       width - 48, 22),
                        &centered, &statusBrush);

    Gdiplus::Pen frame(Gdiplus::Color(255, 48, 53, 64), 1.0f);
    graphics.DrawRectangle(&frame, 0.5f, 0.5f, width - 1.0f, height - 1.0f);
    }

    BitBlt(targetDc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousSurface);
    DeleteObject(surface);
    DeleteDC(dc);
    EndPaint(window, &paint);
}

void DrawLaunchButton(const DRAWITEMSTRUCT& item) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    HBRUSH backdrop = CreateSolidBrush(RGB(10, 13, 19));
    FillRect(item.hDC, &item.rcItem, backdrop);
    DeleteObject(backdrop);
    Gdiplus::Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float width = static_cast<float>(item.rcItem.right - item.rcItem.left);
    const float height = static_cast<float>(item.rcItem.bottom - item.rcItem.top);
    Gdiplus::GraphicsPath shape;
    AddRoundedRect(shape, Gdiplus::RectF(0.5f, 0.5f, width - 1.0f,
                                        height - 1.0f), 8.0f);
    const Gdiplus::Color left = disabled
        ? Gdiplus::Color(255, 55, 58, 67)
        : pressed ? Gdiplus::Color(255, 181, 9, 42)
                  : Gdiplus::Color(255, 235, 12, 55);
    const Gdiplus::Color right = disabled
        ? Gdiplus::Color(255, 42, 45, 53)
        : pressed ? Gdiplus::Color(255, 143, 7, 34)
                  : Gdiplus::Color(255, 188, 9, 44);
    Gdiplus::LinearGradientBrush fill(Gdiplus::PointF(0, 0),
        Gdiplus::PointF(width, 0), left, right);
    graphics.FillPath(&fill, &shape);
    Gdiplus::Pen outline(disabled ? Gdiplus::Color(255, 72, 75, 85)
                                  : Gdiplus::Color(255, 248, 45, 83), 1.0f);
    graphics.DrawPath(&outline, &shape);

    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    Gdiplus::FontFamily family(L"Segoe UI Variable Text");
    Gdiplus::Font font(&family, 14.5f, Gdiplus::FontStyleBold,
                       Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(disabled
        ? Gdiplus::Color(255, 151, 154, 164)
        : Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat centered;
    centered.SetAlignment(Gdiplus::StringAlignmentCenter);
    centered.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    graphics.DrawString(text, -1, &font,
                        Gdiplus::RectF(0, 0, width, height),
                        &centered, &textBrush);
}

LRESULT CALLBACK KeyEditWindowProc(HWND control, UINT message, WPARAM wParam,
                                   LPARAM lParam) {
    if ((message == WM_KEYDOWN && wParam == VK_RETURN) ||
        (message == WM_CHAR && (wParam == L'\r' || wParam == L'\n'))) {
        if (message == WM_KEYDOWN)
            SendMessageW(GetParent(control), WM_COMMAND,
                         MAKEWPARAM(1002, BN_CLICKED),
                         reinterpret_cast<LPARAM>(g_launchButton));
        return 0;
    }
    return CallWindowProcW(g_keyEditProc, control, message, wParam, lParam);
}

int ChromeButtonAt(HWND window, POINT point) {
    RECT client{};
    GetClientRect(window, &client);
    if (point.y < 0 || point.y >= kChromeHeight) return 0;
    if (point.x >= client.right - 40) return 2;
    if (point.x >= client.right - 80) return 1;
    return 0;
}

void InvalidateChromeButtons(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    RECT buttons{client.right - 80, 0, client.right, kChromeHeight};
    InvalidateRect(window, &buttons, FALSE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_bodyFont = CreateFontW(-16, 0, 0, 0, FW_MEDIUM, FALSE, FALSE,
                FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
            g_backgroundBrush = CreateSolidBrush(RGB(10, 13, 19));
            g_editBrush = CreateSolidBrush(RGB(20, 24, 33));
            g_keyEdit = CreateWindowExW(0, L"EDIT", kDefaultLocalKey,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                    ES_AUTOHSCROLL,
                28, 131 + kChromeHeight - kContentLift, 404, 36, window,
                reinterpret_cast<HMENU>(1001),
                nullptr, nullptr);
            SendMessageW(g_keyEdit, WM_SETFONT,
                         reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            SendMessageW(g_keyEdit, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"XXXX-XXXX-XXXX"));
            SendMessageW(g_keyEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                          MAKELPARAM(12, 12));
            RECT keyTextArea{12, 7, 392, 30};
            SendMessageW(g_keyEdit, EM_SETRECTNP, 0,
                         reinterpret_cast<LPARAM>(&keyTextArea));
            g_keyEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                g_keyEdit, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(KeyEditWindowProc)));
            SetWindowRgn(g_keyEdit, CreateRoundRectRgn(0, 0, 404, 36, 8, 8), TRUE);
            g_launchButton = CreateWindowExW(0, L"BUTTON", L"Запустить Axiom",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                24, 190 + kChromeHeight - kContentLift, 412, 44, window,
                reinterpret_cast<HMENU>(1002),
                nullptr, nullptr);
            SendMessageW(g_launchButton, WM_SETFONT,
                          reinterpret_cast<WPARAM>(g_bodyFont), TRUE);
            SetWindowRgn(g_launchButton,
                         CreateRoundRectRgn(0, 0, 412, 44, 10, 10), TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == 1002 && HIWORD(wParam) == BN_CLICKED)
                BeginLaunch(window);
            return 0;
        case WM_DRAWITEM:
            if (wParam == 1002) {
                DrawLaunchButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int hovered = ChromeButtonAt(window, point);
            if (g_chromeHover != hovered) {
                g_chromeHover = hovered;
                InvalidateChromeButtons(window);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!g_chromePressed && g_chromeHover) {
                g_chromeHover = 0;
                InvalidateChromeButtons(window);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            g_chromePressed = ChromeButtonAt(window, point);
            if (g_chromePressed) {
                SetCapture(window);
                InvalidateChromeButtons(window);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP:
            if (g_chromePressed) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const int releasedOver = ChromeButtonAt(window, point);
                const int action = g_chromePressed;
                g_chromePressed = 0;
                ReleaseCapture();
                InvalidateChromeButtons(window);
                if (releasedOver == action) {
                    if (action == 1) ShowWindow(window, SW_MINIMIZE);
                    else DestroyWindow(window);
                }
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (g_chromePressed) {
                g_chromePressed = 0;
                InvalidateChromeButtons(window);
            }
            return 0;
        case WM_NCHITTEST: {
            const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
            if (hit != HTCLIENT) return hit;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            RECT client{};
            GetClientRect(window, &client);
            if (point.y >= 0 && point.y < kChromeHeight &&
                point.x < client.right - 80)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_PAINT:
            PaintLauncher(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(238, 242, 249));
            SetBkColor(dc, RGB(20, 24, 33));
            return reinterpret_cast<LRESULT>(g_editBrush);
        }
        case kLaunchFinished: {
            std::unique_ptr<std::pair<bool, std::wstring>> result(
                reinterpret_cast<std::pair<bool, std::wstring>*>(lParam));
            g_launching = false;
            EnableWindow(g_launchButton, TRUE);
            SetWindowTextW(g_launchButton, L"Запустить Axiom");
            SetStatus(result->second.c_str(), result->first
                ? RGB(86, 214, 142) : RGB(244, 91, 105));
            return 0;
        }
        case WM_DESTROY:
            DeleteObject(g_bodyFont);
            DeleteObject(g_backgroundBrush);
            DeleteObject(g_editBrush);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) !=
        Gdiplus::Ok) return 1;
    LoadEmbeddedBitmap(instance, IDR_AXIOM_LOGO, g_logoStream, g_logoBitmap);
    LoadEmbeddedBitmap(instance, IDR_AXIOM_BACKGROUND, g_backgroundStream,
                       g_backgroundBitmap);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_AXIOM_ICON), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_AXIOM_ICON), IMAGE_ICON, 16, 16, LR_SHARED));
    windowClass.hbrBackground = nullptr;
    if (!RegisterClassExW(&windowClass)) {
        ShutdownBrandGraphics();
        return 1;
    }

    constexpr int width = 460;
    constexpr int height = 285 + kChromeHeight - kContentLift;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kWindowTitle,
        WS_POPUP | WS_MINIMIZEBOX,
        x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) {
        ShutdownBrandGraphics();
        return 1;
    }
    SetWindowRgn(window, CreateRoundRectRgn(0, 0, width, height, 12, 12), TRUE);
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    ShutdownBrandGraphics();
    return static_cast<int>(message.wParam);
}
