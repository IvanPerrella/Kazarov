#include "platform/settings.hpp"

#include <cstdlib>
#include <string>

namespace {

std::string g_env_prefix;

} // namespace

void SettingsSetEnvPrefix(std::string_view prefix) { g_env_prefix.assign(prefix); }

std::string SettingsEnvName(std::string_view name) {
    return g_env_prefix.empty() ? std::string{} : g_env_prefix + "_" + std::string(name);
}

const char *SettingsEnv(std::string_view name) {
    const std::string key = SettingsEnvName(name);
    if (key.empty()) {
        return nullptr;
    }
    return std::getenv(key.c_str());
}
