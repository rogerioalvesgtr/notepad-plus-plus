#ifndef NPP_LINUX_PLUGIN_MANAGER_H
#define NPP_LINUX_PLUGIN_MANAGER_H

#include "npp-linux-plugin.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NppLinux
{
struct PluginCommand
{
	std::string name;
	NppLinuxCommandFunction run = nullptr;
};

struct Plugin
{
	std::filesystem::path file;
	std::string id;          // file name, used to remember whether it is enabled
	bool enabled = false;
	bool loaded = false;
	std::string name;
	std::string version;
	std::string description;
	std::string error;
	std::vector<PluginCommand> commands;
	void *handle = nullptr;
};

class PluginManager
{
public:
	PluginManager() = default;
	~PluginManager();

	PluginManager(const PluginManager &) = delete;
	PluginManager &operator=(const PluginManager &) = delete;

	static std::vector<std::filesystem::path> defaultDirectories();
	// Lists *.so files (name.so or name/name.so) without loading them.
	void scan(const std::vector<std::filesystem::path> &directories, const std::vector<std::string> &enabledIds);
	// Loads the enabled plugins that are not loaded yet.
	void loadEnabled(const NppLinuxHost *host);
	bool load(Plugin &plugin, const NppLinuxHost *host);
	void unloadAll();

	std::vector<Plugin> &plugins() noexcept { return _plugins; }
	const std::vector<Plugin> &plugins() const noexcept { return _plugins; }

private:
	std::vector<Plugin> _plugins;
};
}

#endif
