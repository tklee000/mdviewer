#pragma once

#include <string>
#include <vector>

struct AppConfig {
    int windowWidth = 1280;
    int windowHeight = 820;
    bool maximized = false;
    std::wstring language;
    std::wstring theme = L"dark";
};

const std::vector<std::wstring>& SupportedAppLanguages();
std::wstring NormalizeAppLanguage(std::wstring language);

class ConfigStore {
public:
    ConfigStore();

    AppConfig Load() const;
    bool Save(const AppConfig& config) const;
    const std::wstring& Path() const { return path_; }

private:
    std::wstring path_;
};
