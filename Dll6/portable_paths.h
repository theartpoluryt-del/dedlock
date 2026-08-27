#pragma once

#include <Windows.h>

#include <iterator>
#include <string>

namespace Dll6Paths {

inline std::string DataDirectoryA() {
    static const std::string directory = [] {
        char base[32768]{};
        DWORD length = GetEnvironmentVariableA(
            "LOCALAPPDATA", base, static_cast<DWORD>(std::size(base)));
        if (!length || length >= std::size(base)) {
            length = GetTempPathA(static_cast<DWORD>(std::size(base)), base);
            if (!length || length >= std::size(base))
                return std::string(".");
        }
        std::string result(base, length);
        while (!result.empty() &&
               (result.back() == '\\' || result.back() == '/'))
            result.pop_back();
        result += "\\Axiom";
        CreateDirectoryA(result.c_str(), nullptr);
        return result;
    }();
    return directory;
}

inline std::wstring DataDirectoryW() {
    static const std::wstring directory = [] {
        wchar_t base[32768]{};
        DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA", base, static_cast<DWORD>(std::size(base)));
        if (!length || length >= std::size(base)) {
            length = GetTempPathW(static_cast<DWORD>(std::size(base)), base);
            if (!length || length >= std::size(base))
                return std::wstring(L".");
        }
        std::wstring result(base, length);
        while (!result.empty() &&
               (result.back() == L'\\' || result.back() == L'/'))
            result.pop_back();
        result += L"\\Axiom";
        CreateDirectoryW(result.c_str(), nullptr);
        return result;
    }();
    return directory;
}

inline std::string DataFileA(const char* fileName) {
    return DataDirectoryA() + "\\" + (fileName ? fileName : "");
}

inline std::wstring DataFileW(const wchar_t* fileName) {
    return DataDirectoryW() + L"\\" + (fileName ? fileName : L"");
}

} // namespace Dll6Paths
