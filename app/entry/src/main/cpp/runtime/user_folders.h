#pragma once

namespace ohoswayland {

// The socket in XDG_RUNTIME_DIR where programs ask for a user folder
// (exec_preload.cpp). A request is the folder's name and a newline; the
// answer is one byte, '1' when the user granted it.
constexpr char kFolderSocket[] = "folders";

} // namespace ohoswayland
