#ifndef NPP_LINUX_WORKSPACE_H
#define NPP_LINUX_WORKSPACE_H

#include <filesystem>
#include <string>
#include <vector>

namespace NppLinux
{
struct DirectoryEntry
{
	std::string name;
	std::filesystem::path path;
	bool directory = false;
};

// Folders first, then files, each sorted without regard to case. Hidden
// entries (names starting with ".") are left out unless requested.
std::vector<DirectoryEntry> listDirectory(const std::filesystem::path &folder, bool includeHidden, std::string &error);

// A workspace file lists the root folders of a project.
bool saveWorkspaceFile(const std::filesystem::path &file, const std::vector<std::string> &folders, std::string &error);
std::vector<std::string> loadWorkspaceFile(const std::filesystem::path &file, std::string &error);
}

#endif
