#include "fs_utils.h"

#include <sys/stat.h>

#include <cerrno>
#include <dlfcn.h>

namespace winehua {

bool EnsureDir(const std::string& path)
{
    if (path.empty()) return false;
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string CurrentSharedObjectDir()
{
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<void*>(&CurrentSharedObjectDir), &info) || !info.dli_fname || !info.dli_fname[0])
        return "";
    const std::string path = info.dli_fname;
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

} // namespace winehua
