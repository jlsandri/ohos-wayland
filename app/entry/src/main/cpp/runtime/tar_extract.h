#pragma once

#include <cstdint>
#include <string>

namespace ohoswayland {

// Extracts a GNU or ustar archive that occupies [offset, offset + length) of
// fd (a HAP rawfile from ResourceManager getRawFd) into dest. Regular files,
// directories, symlinks, and GNU long names are supported. Returns the number
// of entries written, or -errno.
int ExtractTar(int fd, int64_t offset, int64_t length, const std::string& dest);

} // namespace ohoswayland
