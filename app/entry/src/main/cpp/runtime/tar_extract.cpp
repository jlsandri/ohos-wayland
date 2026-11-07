#include "runtime/tar_extract.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ohoswayland {
namespace {

constexpr int64_t kBlock = 512;

int64_t ParseOctal(const char* field, size_t size)
{
    int64_t value = 0;
    for (size_t i = 0; i < size && field[i]; ++i) {
        if (field[i] == ' ') continue;
        if (field[i] < '0' || field[i] > '7') break;
        value = value * 8 + (field[i] - '0');
    }
    return value;
}

bool ReadExact(int fd, int64_t offset, void* buffer, size_t size)
{
    auto* out = static_cast<char*>(buffer);
    while (size > 0) {
        const ssize_t got = pread(fd, out, size, offset);
        if (got <= 0) return false;
        out += got;
        offset += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

int MakeParents(const std::string& path)
{
    for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        const std::string parent = path.substr(0, slash);
        if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) return -errno;
    }
    return 0;
}

// Archive paths must stay inside dest.
bool SafeRelative(const std::string& name)
{
    if (name.empty() || name[0] == '/') return false;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

int CopyData(int fd, int64_t offset, int64_t size, const std::string& path, int mode)
{
    unlink(path.c_str());
    const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) return -errno;
    std::vector<char> buffer(256 * 1024);
    while (size > 0) {
        const size_t chunk = static_cast<size_t>(std::min<int64_t>(size, static_cast<int64_t>(buffer.size())));
        if (!ReadExact(fd, offset, buffer.data(), chunk) ||
            write(out, buffer.data(), chunk) != static_cast<ssize_t>(chunk)) {
            const int error = errno ? errno : EIO;
            close(out);
            return -error;
        }
        offset += static_cast<int64_t>(chunk);
        size -= static_cast<int64_t>(chunk);
    }
    close(out);
    return 0;
}

} // namespace

int ExtractTar(int fd, int64_t offset, int64_t length, const std::string& dest)
{
    if (mkdir(dest.c_str(), 0755) != 0 && errno != EEXIST) return -errno;
    const int64_t end = offset + length;
    std::string longName;
    std::string longLink;
    int entries = 0;
    char header[kBlock];
    while (offset + kBlock <= end) {
        if (!ReadExact(fd, offset, header, kBlock)) return -EIO;
        offset += kBlock;
        if (header[0] == '\0') break;
        const int64_t size = ParseOctal(header + 124, 12);
        const int64_t dataOffset = offset;
        offset += (size + kBlock - 1) / kBlock * kBlock;
        const char type = header[156];
        if (type == 'L' || type == 'K') {
            std::string value(static_cast<size_t>(size), '\0');
            if (!ReadExact(fd, dataOffset, value.data(), value.size())) return -EIO;
            value.resize(strnlen(value.c_str(), value.size()));
            (type == 'L' ? longName : longLink) = value;
            continue;
        }
        std::string name = longName;
        if (name.empty()) {
            const std::string prefix(header + 345, strnlen(header + 345, 155));
            name.assign(header, strnlen(header, 100));
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        longName.clear();
        longLink.clear();
        while (name.size() > 1 && name.back() == '/') name.pop_back();
        if (name.rfind("./", 0) == 0) name.erase(0, 2);
        if (name == "." || name.empty()) continue;
        if (!SafeRelative(name)) return -EINVAL;
        const std::string path = dest + "/" + name;
        if (int error = MakeParents(path)) return error;
        const int mode = static_cast<int>(ParseOctal(header + 100, 8) & 0777);
        const int64_t mtime = ParseOctal(header + 136, 12);
        if (type == '5') {
            if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return -errno;
        } else if (type == '0' || type == '\0' || type == '7') {
            // Reinstalls carry mostly unchanged files: skip one whose size and
            // modification time match the entry, which extraction stamps.
            struct stat existing = {};
            if (stat(path.c_str(), &existing) == 0 && S_ISREG(existing.st_mode) && existing.st_size == size &&
                existing.st_mtime == mtime) {
                ++entries;
                continue;
            }
            if (int error = CopyData(fd, dataOffset, size, path, mode ? mode : 0644)) return error;
            const timespec times[2] = {{mtime, 0}, {mtime, 0}};
            utimensat(AT_FDCWD, path.c_str(), times, 0);
        } else {
            // Symlinks and other types are not extracted: ohos_wl.py archives
            // dereference links, and app storage refuses symlinks.
            continue;
        }
        ++entries;
    }
    return entries;
}

} // namespace ohoswayland
