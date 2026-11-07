#pragma once

#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace ohoswayland {

inline void AppendLaunchField(std::string& output, std::string_view value)
{
    output += std::to_string(value.size());
    output.push_back(':');
    output += value;
}

inline bool TakeLaunchField(std::string_view& input, std::string_view& value)
{
    const size_t colon = input.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    size_t size = 0;
    const auto result = std::from_chars(input.data(), input.data() + colon, size);
    if (result.ec != std::errc() || result.ptr != input.data() + colon) return false;
    input.remove_prefix(colon + 1);
    if (input.size() < size) return false;
    value = input.substr(0, size);
    input.remove_prefix(size);
    return true;
}

} // namespace ohoswayland
