#pragma once

#include <filesystem>
#include <string>

namespace AxiomAuth {

inline constexpr wchar_t kLauncherVersion[] = L"1.0.2";

std::wstring LoadSavedLicense();
bool AuthenticateAndAcquireModule(const std::wstring& rawLicense,
                                  std::filesystem::path& modulePath,
                                  std::wstring& error);

}  // namespace AxiomAuth
