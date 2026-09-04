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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "launcher_auth.h"
#include "launcher_resource.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"AxiomLauncherWindow";
constexpr wchar_t kWindowTitle[] = L"Axiom Launcher";
constexpr wchar_t kTargetProcess[] = L"deadlock.exe";
constexpr wchar_t kSteamLaunchUri[] = L"steam://rungameid/1422450";
constexpr int kChromeHeight = 38;
constexpr int kContentLift = 28;
constexpr UINT kLaunchFinished = WM_APP + 1;
constexpr UINT kLaunchProgress = WM_APP + 2;
constexpr DWORD kRemoteCallTimeoutMs = 20000;
// Covers the DLL's 180-second cold-start wait plus schema/hook setup.
constexpr DWORD kInitializerTimeoutMs = 240000;

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
DWORD g_remoteCallExitCode{};
bool g_remoteCallTimedOut{};

struct ScopedHandle {
    HANDLE value{ INVALID_HANDLE_VALUE };
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

bool FitsIn(size_t offset, size_t size, size_t total) {
    return offset <= total && size <= total - offset;
}

bool RvaToFileOffset(const IMAGE_NT_HEADERS64& nt,
                     const IMAGE_SECTION_HEADER* sections, size_t sectionCount,
                     DWORD rva, size_t fileSize, size_t& offset) {
    if (rva < nt.OptionalHeader.SizeOfHeaders) {
        offset = rva;
        return offset < fileSize;
    }
    for (size_t index = 0; index < sectionCount; ++index) {
        const auto& section = sections[index];
        const DWORD sectionSize = (std::max)(section.Misc.VirtualSize,
                                             section.SizeOfRawData);
        if (rva < section.VirtualAddress ||
            rva - section.VirtualAddress >= sectionSize) continue;
        const size_t relative = rva - section.VirtualAddress;
        if (relative >= section.SizeOfRawData) return false;
        offset = static_cast<size_t>(section.PointerToRawData) + relative;
        return offset < fileSize;
    }
    return false;
}

bool ExtractPayloadResources(const std::vector<uint8_t>& dllData,
                             std::wstring& error) {
    if (!FitsIn(0, sizeof(IMAGE_DOS_HEADER), dllData.size())) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(dllData.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
    const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
    if (!FitsIn(ntOffset, sizeof(IMAGE_NT_HEADERS64), dllData.size())) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(dllData.data() + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const size_t sectionOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt->FileHeader.SizeOfOptionalHeader;
    const size_t sectionCount = nt->FileHeader.NumberOfSections;
    if (!FitsIn(sectionOffset, sectionCount * sizeof(IMAGE_SECTION_HEADER), dllData.size()))
        return false;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(dllData.data() + sectionOffset);
    const auto& resourceDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    size_t resourceOffset = 0;
    if (!resourceDirectory.VirtualAddress || !resourceDirectory.Size ||
        !RvaToFileOffset(*nt, sections, sectionCount, resourceDirectory.VirtualAddress,
                         dllData.size(), resourceOffset) ||
        !FitsIn(resourceOffset, resourceDirectory.Size, dllData.size())) {
        error = L"В загруженной DLL не найдены ресурсы интерфейса.";
        return false;
    }
    const auto resourceAt = [&](DWORD offset, size_t size) -> const uint8_t* {
        if (offset > resourceDirectory.Size || size > resourceDirectory.Size - offset)
            return nullptr;
        return dllData.data() + resourceOffset + offset;
    };
    const auto entriesOf = [&](DWORD offset, const IMAGE_RESOURCE_DIRECTORY*& directory,
                               const IMAGE_RESOURCE_DIRECTORY_ENTRY*& entries,
                               DWORD& count) -> bool {
        directory = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY*>(
            resourceAt(offset, sizeof(IMAGE_RESOURCE_DIRECTORY)));
        if (!directory) return false;
        count = directory->NumberOfNamedEntries + directory->NumberOfIdEntries;
        entries = reinterpret_cast<const IMAGE_RESOURCE_DIRECTORY_ENTRY*>(resourceAt(
            offset + sizeof(IMAGE_RESOURCE_DIRECTORY),
            static_cast<size_t>(count) * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY)));
        return entries != nullptr;
    };
    const IMAGE_RESOURCE_DIRECTORY* root = nullptr;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY* rootEntries = nullptr;
    DWORD rootCount = 0;
    if (!entriesOf(0, root, rootEntries, rootCount)) return false;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY* rcdata = nullptr;
    for (DWORD index = root->NumberOfNamedEntries; index < rootCount; ++index) {
        if (!rootEntries[index].NameIsString && rootEntries[index].Id == 10 &&
            rootEntries[index].DataIsDirectory) {
            rcdata = &rootEntries[index];
            break;
        }
    }
    if (!rcdata) {
        error = L"В загруженной DLL отсутствуют данные интерфейса.";
        return false;
    }
    const IMAGE_RESOURCE_DIRECTORY* names = nullptr;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY* nameEntries = nullptr;
    DWORD nameCount = 0;
    if (!entriesOf(rcdata->OffsetToDirectory, names, nameEntries, nameCount)) return false;
    wchar_t localAppData[MAX_PATH]{};
    const DWORD localAppDataLength = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    const auto assetDirectory = std::filesystem::path(
        localAppDataLength && localAppDataLength < std::size(localAppData)
            ? std::wstring(localAppData, localAppDataLength) : std::wstring(L".")) /
        L"Axiom" / L"assets";
    std::error_code fsError;
    std::filesystem::create_directories(assetDirectory, fsError);
    if (fsError) {
        error = L"Не удалось создать папку ресурсов Axiom.";
        return false;
    }
    size_t extracted = 0;
    bool required[4]{};
    for (DWORD index = names->NumberOfNamedEntries; index < nameCount; ++index) {
        const auto& name = nameEntries[index];
        if (name.NameIsString || !name.DataIsDirectory) continue;
        const IMAGE_RESOURCE_DIRECTORY* languages = nullptr;
        const IMAGE_RESOURCE_DIRECTORY_ENTRY* languageEntries = nullptr;
        DWORD languageCount = 0;
        if (!entriesOf(name.OffsetToDirectory, languages, languageEntries, languageCount) ||
            !languageCount || languageEntries[0].DataIsDirectory) continue;
        const auto* data = reinterpret_cast<const IMAGE_RESOURCE_DATA_ENTRY*>(
            resourceAt(languageEntries[0].OffsetToData, sizeof(IMAGE_RESOURCE_DATA_ENTRY)));
        size_t dataOffset = 0;
        if (!data || !data->Size || !RvaToFileOffset(*nt, sections, sectionCount,
            data->OffsetToData, dllData.size(), dataOffset) ||
            !FitsIn(dataOffset, data->Size, dllData.size())) continue;
        const auto finalPath = assetDirectory / (L"res_" + std::to_wstring(name.Id) + L".bin");
        const auto temporaryPath = finalPath.wstring() + L".tmp";
        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output.write(reinterpret_cast<const char*>(dllData.data() + dataOffset), data->Size)) {
                DeleteFileW(temporaryPath.c_str());
                error = L"Не удалось записать ресурс интерфейса.";
                return false;
            }
        }
        if (!MoveFileExW(temporaryPath.c_str(), finalPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporaryPath.c_str());
            error = L"Не удалось обновить ресурс интерфейса.";
            return false;
        }
        ++extracted;
        if (name.Id == 101) required[0] = true;
        if (name.Id == 108) required[1] = true;
        if (name.Id == 300) required[2] = true;
        if (name.Id == 476) required[3] = true;
    }
    if (extracted < 4 || !required[0] || !required[1] || !required[2] || !required[3]) {
        error = L"Не все ресурсы интерфейса удалось подготовить.";
        return false;
    }
    return true;
}

bool ErasePEHeaders(HANDLE process, uintptr_t baseAddress) {
    DWORD oldProtect;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(baseAddress), 0x1000,
        PAGE_READWRITE, &oldProtect)) {
        return false;
    }

    uint8_t zero[0x1000] = {};
    SIZE_T bytesWritten = 0;
    BOOL result = WriteProcessMemory(process, reinterpret_cast<void*>(baseAddress),
        zero, sizeof(zero), &bytesWritten);

    DWORD dummy;
    VirtualProtectEx(process, reinterpret_cast<void*>(baseAddress), 0x1000,
        oldProtect, &dummy);

    return result && bytesWritten == sizeof(zero);
}

struct RemoteCallContext {
    uintptr_t function;
    uintptr_t argument1;
    uint64_t argument2;
    uintptr_t argument3;
    uintptr_t result;
};

bool RemoteCall3(HANDLE process, uintptr_t function, uintptr_t argument1,
                 uint64_t argument2, uintptr_t argument3,
                 uintptr_t& result) {
    g_remoteCallExitCode = 0;
    g_remoteCallTimedOut = false;
    static constexpr uint8_t code[] = {
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
        0x48, 0x8B, 0x03, 0x48, 0x8B, 0x4B, 0x08, 0x48,
        0x8B, 0x53, 0x10, 0x4C, 0x8B, 0x43, 0x18, 0xFF,
        0xD0,
        0x48, 0x89, 0x43, 0x20, 0x48, 0x83, 0xC4, 0x20,
        0x5B, 0xC3
    };
    const SIZE_T contextSize = sizeof(RemoteCallContext);
    const SIZE_T totalSize = contextSize + sizeof(code);
    void* remote = VirtualAllocEx(process, nullptr, totalSize,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
    if (!remote) return false;
    RemoteCallContext context{function, argument1, argument2, argument3, 0};
    const auto cleanup = [&] {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    };
    if (!WriteProcessMemory(process, remote, &context, contextSize, nullptr) ||
        !WriteProcessMemory(process,
            static_cast<uint8_t*>(remote) + contextSize,
            code, sizeof(code), nullptr)) {
        cleanup();
        return false;
    }
    FlushInstructionCache(process,
        static_cast<uint8_t*>(remote) + contextSize, sizeof(code));
    ScopedHandle thread(CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            static_cast<uint8_t*>(remote) + contextSize),
        remote, 0, nullptr));
    if (!thread.valid()) {
        cleanup();
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(thread, kRemoteCallTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        g_remoteCallTimedOut = true;
        // The thread may still execute the trampoline. Its memory must remain
        // valid until the target process exits.
        return false;
    }
    GetExitCodeThread(thread, &g_remoteCallExitCode);
    if (!ReadProcessMemory(process, remote, &context, contextSize, nullptr)) {
        cleanup();
        return false;
    }
    result = context.result;
    cleanup();
    return true;
}

uintptr_t RemoteAddressForLocal(DWORD processId, const void* address) {
    HMODULE owner{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &owner)) return 0;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(owner, path, static_cast<DWORD>(std::size(path))))
        return 0;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    const uintptr_t remoteBase = FindRemoteModule(processId, name);
    if (!remoteBase) return 0;
    return remoteBase + reinterpret_cast<uintptr_t>(address) -
           reinterpret_cast<uintptr_t>(owner);
}

uintptr_t RemoteLoadDependency(HANDLE process, DWORD processId,
                               const char* moduleName) {
    wchar_t wideName[MAX_PATH]{};
    const int converted = MultiByteToWideChar(
        CP_ACP, 0, moduleName, -1, wideName, static_cast<int>(std::size(wideName)));
    if (converted > 0) {
        const uintptr_t existing = FindRemoteModule(processId, wideName);
        if (existing) return existing;
    }
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const FARPROC loadLibrary = kernel
        ? GetProcAddress(kernel, "LoadLibraryA") : nullptr;
    const uintptr_t remoteLoadLibrary = loadLibrary
        ? RemoteAddressForLocal(processId, loadLibrary) : 0;
    if (!remoteLoadLibrary) return 0;
    const SIZE_T bytes = strlen(moduleName) + 1;
    void* remoteName = VirtualAllocEx(process, nullptr, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteName) return 0;
    if (!WriteProcessMemory(process, remoteName, moduleName, bytes, nullptr)) {
        VirtualFreeEx(process, remoteName, 0, MEM_RELEASE);
        return 0;
    }
    uintptr_t result = 0;
    const bool called = RemoteCall3(process, remoteLoadLibrary,
        reinterpret_cast<uintptr_t>(remoteName), 0, 0, result);
    VirtualFreeEx(process, remoteName, 0, MEM_RELEASE);
    return called ? result : 0;
}

bool ReadRemoteString(HANDLE process, uintptr_t address, std::string& value) {
    value.clear();
    for (size_t offset = 0; offset < 1024; offset += 128) {
        char block[128]{};
        SIZE_T received = 0;
        if (!ReadProcessMemory(process,
                reinterpret_cast<void*>(address + offset), block,
                sizeof(block), &received) || !received) return false;
        const char* end = static_cast<const char*>(
            memchr(block, '\0', received));
        value.append(block, end ? static_cast<size_t>(end - block) : received);
        if (end) return true;
    }
    return false;
}

uintptr_t ResolveRemoteExport(HANDLE process, DWORD processId,
                              uintptr_t moduleBase, const char* functionName,
                              WORD ordinal, bool byOrdinal, int depth = 0) {
    if (!moduleBase || depth > 8) return 0;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(moduleBase),
                           &dos, sizeof(dos), nullptr) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !ReadProcessMemory(process,
            reinterpret_cast<void*>(moduleBase + dos.e_lfanew),
            &nt, sizeof(nt), nullptr) || nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;
    const auto& directory =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress ||
        directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return 0;
    IMAGE_EXPORT_DIRECTORY exports{};
    if (!ReadProcessMemory(process,
            reinterpret_cast<void*>(moduleBase + directory.VirtualAddress),
            &exports, sizeof(exports), nullptr) ||
        !exports.NumberOfFunctions || exports.NumberOfFunctions > 1'000'000 ||
        exports.NumberOfNames > exports.NumberOfFunctions)
        return 0;
    std::vector<DWORD> functions(exports.NumberOfFunctions);
    if (!ReadProcessMemory(process,
            reinterpret_cast<void*>(moduleBase + exports.AddressOfFunctions),
            functions.data(), functions.size() * sizeof(DWORD), nullptr))
        return 0;
    DWORD functionIndex = UINT32_MAX;
    if (byOrdinal) {
        if (ordinal < exports.Base ||
            static_cast<DWORD>(ordinal - exports.Base) >= exports.NumberOfFunctions)
            return 0;
        functionIndex = ordinal - exports.Base;
    } else {
        std::vector<DWORD> names(exports.NumberOfNames);
        std::vector<WORD> ordinals(exports.NumberOfNames);
        if (!ReadProcessMemory(process,
                reinterpret_cast<void*>(moduleBase + exports.AddressOfNames),
                names.data(), names.size() * sizeof(DWORD), nullptr) ||
            !ReadProcessMemory(process,
                reinterpret_cast<void*>(moduleBase + exports.AddressOfNameOrdinals),
                ordinals.data(), ordinals.size() * sizeof(WORD), nullptr))
            return 0;
        std::string candidate;
        for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
            if (!ReadRemoteString(process, moduleBase + names[i], candidate))
                return 0;
            if (candidate == functionName) {
                functionIndex = ordinals[i];
                break;
            }
        }
        if (functionIndex == UINT32_MAX ||
            functionIndex >= exports.NumberOfFunctions) return 0;
    }
    const DWORD functionRva = functions[functionIndex];
    if (!functionRva) return 0;
    const uint64_t exportEnd = static_cast<uint64_t>(directory.VirtualAddress) +
                               directory.Size;
    if (functionRva >= directory.VirtualAddress && functionRva < exportEnd) {
        std::string forwarder;
        if (!ReadRemoteString(process, moduleBase + functionRva, forwarder))
            return 0;
        const size_t separator = forwarder.rfind('.');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= forwarder.size()) return 0;
        std::string forwardedModule = forwarder.substr(0, separator);
        if (forwardedModule.find('.') == std::string::npos)
            forwardedModule += ".dll";
        const std::string forwardedName = forwarder.substr(separator + 1);
        const uintptr_t forwardedBase = RemoteLoadDependency(
            process, processId, forwardedModule.c_str());
        if (!forwardedBase) return 0;
        if (forwardedName[0] == '#') {
            const unsigned long value = strtoul(forwardedName.c_str() + 1,
                                                nullptr, 10);
            if (!value || value > 0xFFFF) return 0;
            return ResolveRemoteExport(process, processId, forwardedBase,
                                       nullptr, static_cast<WORD>(value), true,
                                       depth + 1);
        }
        return ResolveRemoteExport(process, processId, forwardedBase,
                                   forwardedName.c_str(), 0, false, depth + 1);
    }
    return moduleBase + functionRva;
}

DWORD SectionProtection(DWORD characteristics) {
    const bool execute = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool read = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    const bool write = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    if (execute) {
        if (write) return PAGE_EXECUTE_READWRITE;
        if (read) return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (write) return PAGE_READWRITE;
    if (read) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

bool ManualMapInject(HANDLE process, const uint8_t* dllData, size_t dllSize,
                     uintptr_t& baseAddress, std::wstring& detail) {
    baseAddress = 0;
    detail.clear();
    const auto reject = [&](const wchar_t* stage) {
        detail = L"Manual Map: " + std::wstring(stage);
        return false;
    };
    if (!dllData || !FitsIn(0, sizeof(IMAGE_DOS_HEADER), dllSize))
        return reject(L"проверка DOS-заголовка");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(dllData);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        return reject(L"проверка DOS-заголовка");
    const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
    if (!FitsIn(ntOffset, sizeof(IMAGE_NT_HEADERS64), dllSize))
        return reject(L"проверка NT-заголовка");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(dllData + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
        return reject(L"проверка формата PE64");
    const auto& opt = nt->OptionalHeader;
    if (!opt.SizeOfImage || opt.SizeOfHeaders > opt.SizeOfImage ||
        !FitsIn(0, opt.SizeOfHeaders, dllSize))
        return reject(L"проверка размера образа");
    const size_t sectionOffset = ntOffset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                 nt->FileHeader.SizeOfOptionalHeader;
    const size_t sectionBytes = static_cast<size_t>(nt->FileHeader.NumberOfSections) *
                                sizeof(IMAGE_SECTION_HEADER);
    if (!FitsIn(sectionOffset, sectionBytes, dllSize))
        return reject(L"проверка таблицы секций");
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        dllData + sectionOffset);

    std::vector<uint8_t> image(opt.SizeOfImage, 0);
    memcpy(image.data(), dllData, opt.SizeOfHeaders);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        const size_t mappedSize = (std::max)(
            static_cast<size_t>(section.Misc.VirtualSize),
            static_cast<size_t>(section.SizeOfRawData));
        if (mappedSize &&
            !FitsIn(section.VirtualAddress, mappedSize, image.size()))
            return reject(L"проверка виртуального размера секции");
        if (section.SizeOfRawData) {
            if (!FitsIn(section.PointerToRawData, section.SizeOfRawData, dllSize) ||
                !FitsIn(section.VirtualAddress, section.SizeOfRawData, image.size()))
                return reject(L"проверка данных секции");
            memcpy(image.data() + section.VirtualAddress,
                   dllData + section.PointerToRawData, section.SizeOfRawData);
        }
    }

    baseAddress = reinterpret_cast<uintptr_t>(VirtualAllocEx(process,
        reinterpret_cast<void*>(opt.ImageBase), opt.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!baseAddress) {
        baseAddress = reinterpret_cast<uintptr_t>(VirtualAllocEx(process,
            nullptr, opt.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    if (!baseAddress) return reject(L"выделение памяти");
    std::wstring stage = L"подготовка образа";
    bool exceptionRegistered = false;
    uintptr_t exceptionTable = 0;
    uintptr_t remoteDeleteFunctionTable = 0;
    const auto fail = [&] {
        if (g_remoteCallTimedOut) {
            baseAddress = 0;
            detail = L"Manual Map: " + stage + L" (таймаут 20 с)";
            return false;
        }
        if (exceptionRegistered && remoteDeleteFunctionTable) {
            uintptr_t ignored = 0;
            RemoteCall3(process, remoteDeleteFunctionTable,
                        exceptionTable, 0, 0, ignored);
        }
        VirtualFreeEx(process, reinterpret_cast<void*>(baseAddress), 0,
                      MEM_RELEASE);
        baseAddress = 0;
        detail = L"Manual Map: " + stage;
        return false;
    };

    stage = L"релокации";
    const uintptr_t delta = baseAddress - opt.ImageBase;
    if (delta) {
        const auto& directory = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (!directory.VirtualAddress || !directory.Size ||
            !FitsIn(directory.VirtualAddress, directory.Size, image.size()))
            return fail();
        size_t offset = 0;
        while (offset < directory.Size) {
            if (!FitsIn(directory.VirtualAddress + offset,
                        sizeof(IMAGE_BASE_RELOCATION), image.size())) return fail();
            auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                image.data() + directory.VirtualAddress + offset);
            if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
                block->SizeOfBlock > directory.Size - offset) return fail();
            const size_t count = (block->SizeOfBlock -
                                  sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            const auto* entries = reinterpret_cast<const WORD*>(block + 1);
            for (size_t i = 0; i < count; ++i) {
                const WORD type = entries[i] >> 12;
                const DWORD rva = block->VirtualAddress + (entries[i] & 0x0FFF);
                if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
                if (type != IMAGE_REL_BASED_DIR64 ||
                    !FitsIn(rva, sizeof(uint64_t), image.size())) return fail();
                *reinterpret_cast<uint64_t*>(image.data() + rva) += delta;
            }
            offset += block->SizeOfBlock;
        }
    }

    const DWORD processId = GetProcessId(process);
    stage = L"импорты";
    const auto& importDirectory = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDirectory.VirtualAddress) {
        if (importDirectory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
            !FitsIn(importDirectory.VirtualAddress,
                    importDirectory.Size, image.size())) return fail();
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            image.data() + importDirectory.VirtualAddress);
        for (;;) {
            const size_t descriptorOffset = reinterpret_cast<uint8_t*>(descriptor) -
                                            image.data();
            if (descriptorOffset < importDirectory.VirtualAddress ||
                !FitsIn(descriptorOffset, sizeof(*descriptor),
                        static_cast<size_t>(importDirectory.VirtualAddress) +
                            importDirectory.Size))
                return fail();
            if (!descriptor->Name) break;
            if (!FitsIn(descriptor->Name, 1, image.size())) return fail();
            const char* moduleName = reinterpret_cast<const char*>(
                image.data() + descriptor->Name);
            const size_t remaining = image.size() - descriptor->Name;
            if (!memchr(moduleName, '\0', remaining)) return fail();
            const std::wstring importModule(moduleName,
                moduleName + strlen(moduleName));
            stage = L"импорты: " + importModule;
            const uintptr_t remoteModule = RemoteLoadDependency(
                process, processId, moduleName);
            if (!remoteModule) return fail();
            const DWORD lookupRva = descriptor->OriginalFirstThunk
                ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
            size_t index = 0;
            bool importsOk = true;
            for (;;) {
                const size_t lookupOffset = lookupRva + index * sizeof(IMAGE_THUNK_DATA64);
                const size_t iatOffset = descriptor->FirstThunk +
                                         index * sizeof(IMAGE_THUNK_DATA64);
                if (!FitsIn(lookupOffset, sizeof(IMAGE_THUNK_DATA64), image.size()) ||
                    !FitsIn(iatOffset, sizeof(IMAGE_THUNK_DATA64), image.size())) {
                    importsOk = false;
                    break;
                }
                const auto* lookup = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                    image.data() + lookupOffset);
                if (!lookup->u1.AddressOfData) break;
                std::wstring importName;
                bool byOrdinal = false;
                WORD importOrdinal = 0;
                const char* functionName = nullptr;
                if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) {
                    byOrdinal = true;
                    importOrdinal = static_cast<WORD>(
                        IMAGE_ORDINAL64(lookup->u1.Ordinal));
                    importName = L"#" + std::to_wstring(importOrdinal);
                } else {
                    const size_t nameOffset = static_cast<size_t>(lookup->u1.AddressOfData);
                    if (!FitsIn(nameOffset, sizeof(WORD) + 1, image.size())) {
                        importsOk = false;
                        break;
                    }
                    functionName = reinterpret_cast<const char*>(
                        image.data() + nameOffset + sizeof(WORD));
                    if (!memchr(functionName, '\0', image.size() - nameOffset - sizeof(WORD))) {
                        importsOk = false;
                        break;
                    }
                    importName.assign(functionName,
                                      functionName + strlen(functionName));
                }
                stage = L"импорты: " + importModule + L"!" + importName;
                const uintptr_t remoteFunction = ResolveRemoteExport(
                    process, processId, remoteModule, functionName,
                    importOrdinal, byOrdinal);
                if (!remoteFunction) {
                    importsOk = false;
                    break;
                }
                reinterpret_cast<IMAGE_THUNK_DATA64*>(image.data() + iatOffset)
                    ->u1.Function = remoteFunction;
                ++index;
            }
            if (!importsOk) return fail();
            ++descriptor;
        }
    }

    stage = L"запись образа";
    if (!WriteProcessMemory(process, reinterpret_cast<void*>(baseAddress),
                            image.data(), image.size(), nullptr)) return fail();

    stage = L"защита секций";
    DWORD oldProtection{};
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(baseAddress),
                          opt.SizeOfHeaders, PAGE_READONLY, &oldProtection))
        return fail();
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        const SIZE_T size = section.Misc.VirtualSize
            ? section.Misc.VirtualSize : section.SizeOfRawData;
        if (!size) continue;
        if (!VirtualProtectEx(process,
            reinterpret_cast<void*>(baseAddress + section.VirtualAddress),
            size, SectionProtection(section.Characteristics), &oldProtection))
            return fail();
    }
    FlushInstructionCache(process, reinterpret_cast<void*>(baseAddress),
                          opt.SizeOfImage);

    stage = L"таблица исключений";
    const auto& exceptionDirectory =
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exceptionDirectory.VirtualAddress && exceptionDirectory.Size) {
        if (!FitsIn(exceptionDirectory.VirtualAddress,
                    exceptionDirectory.Size, image.size()) ||
            exceptionDirectory.Size % sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) != 0)
            return fail();
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const FARPROC addFunctionTable = ntdll
            ? GetProcAddress(ntdll, "RtlAddFunctionTable") : nullptr;
        const uintptr_t remoteAddFunctionTable = addFunctionTable
            ? RemoteAddressForLocal(processId, addFunctionTable) : 0;
        const FARPROC deleteFunctionTable = ntdll
            ? GetProcAddress(ntdll, "RtlDeleteFunctionTable") : nullptr;
        remoteDeleteFunctionTable = deleteFunctionTable
            ? RemoteAddressForLocal(processId, deleteFunctionTable) : 0;
        exceptionTable = baseAddress + exceptionDirectory.VirtualAddress;
        uintptr_t registered = 0;
        if (!remoteAddFunctionTable || !remoteDeleteFunctionTable ||
            !RemoteCall3(process, remoteAddFunctionTable,
                exceptionTable,
                exceptionDirectory.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY),
                baseAddress, registered) || !registered) return fail();
        exceptionRegistered = true;
    }

    stage = L"TLS callbacks";
    const auto& tlsDirectory = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsDirectory.VirtualAddress) {
        if (!FitsIn(tlsDirectory.VirtualAddress,
                    sizeof(IMAGE_TLS_DIRECTORY64), image.size())) return fail();
        const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
            image.data() + tlsDirectory.VirtualAddress);
        if (tls->AddressOfCallBacks) {
            if (tls->AddressOfCallBacks < baseAddress) return fail();
            const size_t callbackOffset = static_cast<size_t>(
                tls->AddressOfCallBacks - baseAddress);
            for (size_t index = 0;; ++index) {
                const size_t slot = callbackOffset + index * sizeof(uintptr_t);
                if (!FitsIn(slot, sizeof(uintptr_t), image.size())) return fail();
                const uintptr_t callback = *reinterpret_cast<const uintptr_t*>(
                    image.data() + slot);
                if (!callback) break;
                uintptr_t callbackResult = 0;
                if (!RemoteCall3(process, callback, baseAddress,
                                 DLL_PROCESS_ATTACH, 0, callbackResult))
                    return fail();
            }
        }
    }

    stage = L"точка входа DLL";
    if (opt.AddressOfEntryPoint) {
        if (!FitsIn(opt.AddressOfEntryPoint, 1, image.size())) return fail();
        uintptr_t entryResult = 0;
        if (!RemoteCall3(process, baseAddress + opt.AddressOfEntryPoint,
                         baseAddress, DLL_PROCESS_ATTACH, 0, entryResult) ||
            !entryResult) {
            wchar_t code[32]{};
            swprintf_s(code, L" (0x%08X)", g_remoteCallExitCode);
            stage += code;
            return fail();
        }
    }

    stage = L"инициализация DLL";
    const uintptr_t initializer = ResolveRemoteExport(
        process, processId, baseAddress, "AxiomManualMapInitialize",
        0, false);
    if (initializer) {
        ScopedHandle initializeThread(CreateRemoteThread(
            process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(initializer),
            nullptr, 0, nullptr));
        if (!initializeThread.valid())
            return fail();
        const DWORD waitResult = WaitForSingleObject(
            initializeThread, kInitializerTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
            // Do not unmap code that the timed-out thread may still execute.
            baseAddress = 0;
            detail = waitResult == WAIT_TIMEOUT
                ? L"Игра не завершила подготовку Axiom за 240 секунд."
                : L"Manual Map: ошибка ожидания инициализации DLL";
            return false;
        }
        DWORD initializeCode = 0;
        if (!GetExitCodeThread(initializeThread, &initializeCode) ||
            initializeCode >= 0xC0000000) {
            wchar_t code[32]{};
            swprintf_s(code, L" (0x%08X)", initializeCode);
            stage += code;
            return fail();
        }
        if (initializeCode != ERROR_SUCCESS) {
            detail = initializeCode == ERROR_NOT_READY
                ? L"Система объектов игры не готова. Подробности: Axiom/offset_resolution_failed.log."
                : L"DLL не смогла завершить подготовку. Перезапустите игру и попробуйте снова.";
            return false;
        }
    }

    return true;
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

uintptr_t ReadManualMapBase(DWORD processId, ScopedHandle& mapping) {
    const std::wstring name = L"Local\\Dll6_Deadlock_ManualMap_" +
                              std::to_wstring(processId);
    mapping.value = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping.valid()) return 0;
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0,
                                     sizeof(uintptr_t));
    if (!view) return 0;
    const uintptr_t base = *static_cast<const uintptr_t*>(view);
    UnmapViewOfFile(view);
    return base;
}

bool ReleaseManualMappedImage(DWORD processId, uintptr_t baseAddress) {
    ScopedHandle process(OpenProcess(PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId));
    if (!process.valid()) return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(baseAddress),
                           &dos, sizeof(dos), nullptr) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !ReadProcessMemory(process,
            reinterpret_cast<void*>(baseAddress + dos.e_lfanew),
            &nt, sizeof(nt), nullptr) || nt.Signature != IMAGE_NT_SIGNATURE)
        return false;
    const auto& exceptionDirectory =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exceptionDirectory.VirtualAddress && exceptionDirectory.Size) {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const FARPROC deleteFunctionTable = ntdll
            ? GetProcAddress(ntdll, "RtlDeleteFunctionTable") : nullptr;
        const uintptr_t remoteDelete = deleteFunctionTable
            ? RemoteAddressForLocal(processId, deleteFunctionTable) : 0;
        uintptr_t removed = 0;
        if (!remoteDelete || !RemoteCall3(process, remoteDelete,
                baseAddress + exceptionDirectory.VirtualAddress,
                0, 0, removed) || !removed) return false;
    }
    return VirtualFreeEx(process, reinterpret_cast<void*>(baseAddress), 0,
                         MEM_RELEASE) != FALSE;
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
    ScopedHandle manualMapInfo;
    const uintptr_t manualMapBase = ReadManualMapBase(processId, manualMapInfo);
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
        if (!IsPayloadLoaded(processId)) {
            if (manualMapBase) {
                Sleep(100);
                if (!ReleaseManualMappedImage(processId, manualMapBase)) {
                    error = L"DLL завершила работу, но её образ не удалось освободить.";
                    return false;
                }
            }
            return true;
        }
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

    // Читаем DLL в память
    std::ifstream file(dllPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        error = L"Не удалось открыть DLL-файл.";
        return false;
    }
    std::streamsize size = file.tellg();
    if (size <= 0 || size > 512ll * 1024 * 1024) {
        error = L"DLL-файл имеет некорректный размер.";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> dllData(size);
    if (!file.read(reinterpret_cast<char*>(dllData.data()), size)) {
        error = L"Не удалось прочитать DLL-файл.";
        return false;
    }

    // Ресурсы извлекаются из проверенной доставленной DLL до manual map.
    // После очистки PE-заголовков DLL читает только эти внешние файлы.
    if (!ExtractPayloadResources(dllData, error)) {
        if (error.empty()) error = L"Не удалось подготовить ресурсы интерфейса.";
        return false;
    }

    // Manual Map инжект
    uintptr_t baseAddress = 0;
    if (!ManualMapInject(process, dllData.data(), dllData.size(), baseAddress,
                         error)) {
        if (error.empty()) error = L"Manual Map инжект не удался.";
        return false;
    }

    ErasePEHeaders(process, baseAddress);

    // Ждём готовности DLL (по событию)
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

void PostLaunchProgress(HWND window, const wchar_t* text,
                        COLORREF color = RGB(250, 194, 78)) {
    auto* status = new std::wstring(text ? text : L"");
    if (!PostMessageW(window, kLaunchProgress,
                      static_cast<WPARAM>(color),
                      reinterpret_cast<LPARAM>(status)))
        delete status;
}

void BeginLaunch(HWND window) {
    if (g_launching) return;
    const int length = GetWindowTextLengthW(g_keyEdit);
    std::wstring key(static_cast<size_t>((std::max)(length, 0)) + 1, L'\0');
    if (length > 0) GetWindowTextW(g_keyEdit, key.data(), length + 1);
    key.resize(static_cast<size_t>((std::max)(length, 0)));
#ifndef AXIOM_OFFLINE_TEST_MODE
    if (key.empty()) {
        SetStatus(L"Введите лицензионный ключ", RGB(244, 91, 105));
        return;
    }
#endif

    g_launching = true;
    EnableWindow(g_launchButton, FALSE);
    SetWindowTextW(g_launchButton, L"Запуск...");
#ifdef AXIOM_OFFLINE_TEST_MODE
    SetStatus(L"Подготовка DLL и ожидание Deadlock...", RGB(250, 194, 78));
#else
    SetStatus(L"Проверка лицензии...", RGB(250, 194, 78));
#endif
    std::thread([window, key = std::move(key)] {
        std::wstring message;
        std::filesystem::path payload;
        bool success = AxiomAuth::AuthenticateAndAcquireModule(
            key, payload, message);
        DWORD processId = 0;
        if (success) {
            processId = FindProcessId(kTargetProcess);
            if (!processId)
                PostLaunchProgress(window, L"Запуск Deadlock...");
            else if (!IsGameRuntimeReady(processId))
                PostLaunchProgress(window, L"Ожидание загрузки Deadlock...");
            processId = WaitForDeadlock(message);
        }
        success = success && processId != 0;
        if (success) {
            PostLaunchProgress(window, L"Инициализация Axiom...");
            success = LoadPayload(processId, payload, message);
        }
        if (!payload.empty() && !DeleteFileW(payload.c_str())) {
            MoveFileExW(payload.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
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

    Gdiplus::FontFamily brandFamily(L"Segoe UI");
    Gdiplus::Font brandFont(&brandFamily, 29.0f, Gdiplus::FontStyleBold,
                            Gdiplus::UnitPixel);
    Gdiplus::FontFamily textFamily(L"Segoe UI");
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

    Gdiplus::Font versionFont(&textFamily, 10.0f,
                              Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush versionBrush(Gdiplus::Color(255, 91, 97, 109));
    Gdiplus::StringFormat versionFormat;
    versionFormat.SetAlignment(Gdiplus::StringAlignmentFar);
    versionFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    const std::wstring versionText =
        std::wstring(L"v") + AxiomAuth::kLauncherVersion;
    graphics.DrawString(versionText.c_str(), -1, &versionFont,
                        Gdiplus::RectF(width - 90.0f, height - 17.0f,
                                       78.0f, 13.0f),
                        &versionFormat, &versionBrush);

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
    Gdiplus::FontFamily family(L"Segoe UI");
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
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_backgroundBrush = CreateSolidBrush(RGB(10, 13, 19));
            g_editBrush = CreateSolidBrush(RGB(20, 24, 33));
            const std::wstring savedLicense = AxiomAuth::LoadSavedLicense();
            g_keyEdit = CreateWindowExW(0, L"EDIT", savedLicense.c_str(),
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
        case kLaunchProgress: {
            std::unique_ptr<std::wstring> status(
                reinterpret_cast<std::wstring*>(lParam));
            SetStatus(status->c_str(), static_cast<COLORREF>(wParam));
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
