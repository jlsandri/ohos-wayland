#pragma once

namespace ohoswayland {

// Routes the loaded object's libc imports through HongMeng workarounds.
void ApplyCompatibilityShims(const char* path);

// Runs a dlopen'ed position-independent executable that does not export main.
// The executable's own startup code finds main; its __libc_start_main call is
// redirected so main runs here and returns. Sets *ran to false when the
// object cannot be entered this way. Returns main's status.
int RunExecutableEntry(const char* path, int argc, char** argv, char** envp, bool* ran);

} // namespace ohoswayland
