#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ohoswayland {

struct AppRecord {
    std::string appId;
    std::string packagePath;
    std::string entryPoint;
    std::vector<std::string> args;
    std::vector<std::string> env;
    std::vector<int32_t> pids;
};

} // namespace ohoswayland
