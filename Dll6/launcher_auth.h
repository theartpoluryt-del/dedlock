#pragma once

#include <filesystem>
#include <string>

namespace AxiomAuth {

std::wstring LoadSavedLicense();
bool AuthenticateAndAcquireModule(const std::wstring& rawLicense,
                                  std::filesystem::path& modulePath,
                                  std::wstring& error);

}  // namespace AxiomAuth
