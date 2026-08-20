#pragma once

#include <windows.h>

#include <initializer_list>
#include <string>
#include <utility>

namespace localization {

using Parameters =
    std::initializer_list<std::pair<std::string, std::string>>;

std::string Text(HINSTANCE instance, const std::wstring& locale,
                 const std::string& english, Parameters parameters = {});
std::wstring Text(HINSTANCE instance, const std::wstring& locale,
                  const std::wstring& english);

}  // namespace localization
