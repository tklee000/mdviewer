#pragma once

#include <optional>
#include <string>

namespace json {

std::string Escape(const std::string& value);
std::string Quote(const std::string& value);
std::optional<std::string> GetString(const std::string& object,
                                     const std::string& key);
std::optional<bool> GetBool(const std::string& object,
                            const std::string& key);
std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value, bool allowAnsiFallback = true);
bool IsValidUtf8(const std::string& value);

}  // namespace json
