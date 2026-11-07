#pragma once

#include "runtime/app_record.h"
#include <mutex>
#include <string>
#include <unordered_map>

namespace ohoswayland {

class NativeChildLauncher {
public:
    static NativeChildLauncher& Instance();
    int32_t Launch(AppRecord record, std::string& error);
    void Stop(const std::string& appId, std::string& error);
    void OnExit(int32_t pid);

private:
    NativeChildLauncher() = default;
    std::mutex mutex_;
    std::unordered_map<std::string, AppRecord> apps_;
};

} // namespace ohoswayland
