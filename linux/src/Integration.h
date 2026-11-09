#ifndef NPP_LINUX_INTEGRATION_H
#define NPP_LINUX_INTEGRATION_H

#include <filesystem>
#include <string>
#include <vector>

namespace NppLinux
{
// One editor per user, as in the Windows version: a second start hands its
// files to the running editor through a socket in the user's runtime folder
// (only the user can reach it) and exits.
std::filesystem::path instanceSocketPath();
// Returns true when a running editor received the files (or was asked to
// come to the front when there are none).
bool sendToRunningInstance(const std::filesystem::path &socket, const std::vector<std::filesystem::path> &files);
}

#endif
