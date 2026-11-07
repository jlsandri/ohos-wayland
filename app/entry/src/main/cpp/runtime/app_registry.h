#pragma once

#include "runtime/app_record.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ohoswayland {

class AppRegistry {
public:
    static AppRegistry& Instance();
    void Register(AppRecord record);
    std::vector<AppRecord> List();
    AppRecord Find(const std::string& appId);

private:
    AppRegistry() = default;
    void EnsureDefaults();
    mutable std::mutex mutex_;
    std::unordered_map<std::string, AppRecord> apps_;
    bool defaultsRegistered_ = false;
};

} // namespace ohoswayland
