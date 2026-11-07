#include "app_registry.h"

#include <stdexcept>
#include <utility>

namespace ohoswayland {

AppRegistry& AppRegistry::Instance()
{
    static AppRegistry registry;
    return registry;
}

void AppRegistry::EnsureDefaults()
{
    if (defaultsRegistered_) return;
    defaultsRegistered_ = true;
    Register({"native-wl-shm-demo", "libnative_demo.so", "OhosWaylandDemoMain", {}, {}});
    Register({"native-wl-shm-demo-second", "libnative_demo.so", "OhosWaylandDemoMain", {}, {}});
    Register({"native-virgl-gl-demo", "libnative_gl_demo.so", "OhosWaylandGlDemoMain", {}, {}});
    Register({"native-gtk-demo", "libnative_gtk_demo.so", "OhosWaylandGtkDemoMain", {}, {}});
    Register({"ghostty", "libnative_ghostty.so", "OhosWaylandGhosttyMain", {}, {}});
}

void AppRegistry::Register(AppRecord record)
{
    if (record.appId.empty() || record.packagePath.empty() || record.entryPoint.empty()) {
        throw std::invalid_argument("app id, package path, and entry point are required");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    apps_[record.appId] = std::move(record);
}

std::vector<AppRecord> AppRegistry::List()
{
    EnsureDefaults();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AppRecord> result;
    result.reserve(apps_.size());
    for (const auto& [appId, record] : apps_) result.push_back(record);
    return result;
}

AppRecord AppRegistry::Find(const std::string& appId)
{
    EnsureDefaults();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = apps_.find(appId);
    if (it == apps_.end()) throw std::out_of_range("unknown app id: " + appId);
    return it->second;
}

} // namespace ohoswayland
