#pragma once

#include <string>

namespace winehua {

bool EnsureDir(const std::string& path);
std::string CurrentSharedObjectDir();

} // namespace winehua
