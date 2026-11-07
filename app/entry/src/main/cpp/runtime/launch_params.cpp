#include "launch_params.h"

#include "launch_protocol.h"
#include <cstdlib>
#include <string>
#include <string_view>

namespace ohoswayland {

bool ApplyLaunchParams(const char* encoded)
{
    std::string_view input = encoded ? encoded : "";
    std::string_view value;
    for (int i = 0; i < 4; ++i) {
        if (!TakeLaunchField(input, value)) return false;
    }
    if (!TakeLaunchField(input, value)) return false;
    size_t count = 0;
    if (std::from_chars(value.data(), value.data() + value.size(), count).ec != std::errc()) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!TakeLaunchField(input, value)) return false;
    }
    if (!TakeLaunchField(input, value)) return false;
    if (std::from_chars(value.data(), value.data() + value.size(), count).ec != std::errc()) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!TakeLaunchField(input, value)) return false;
        const size_t equal = value.find('=');
        if (equal == 0 || equal == std::string_view::npos) continue;
        const std::string key(value.substr(0, equal));
        const std::string envValue(value.substr(equal + 1));
        if (setenv(key.c_str(), envValue.c_str(), 1) != 0) return false;
    }
    return input.empty();
}

} // namespace ohoswayland
