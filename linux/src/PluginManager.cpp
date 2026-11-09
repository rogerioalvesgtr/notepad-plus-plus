#include "PluginManager.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <system_error>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NPP_LINUX_PLUGIN_DIR
#define NPP_LINUX_PLUGIN_DIR "/usr/local/lib/notepad-plus-plus-linux/plugins"
#endif

namespace NppLinux
{
namespace
{
constexpr int maxCommands = 100;

void addCandidate(std::vector<std::filesystem::path> &files, const std::filesystem::path &path)
{
	std::error_code error;
	if (path.extension() == ".so" && std::filesystem::is_regular_file(path, error))
		files.push_back(path);
}
}

PluginManager::~PluginManager()
{
	unloadAll();
}

std::vector<std::filesystem::path> PluginManager::defaultDirectories()
{
	std::vector<std::filesystem::path> directories;
	if (const char *data = std::getenv("XDG_DATA_HOME"); data && *data)
		directories.emplace_back(std::filesystem::path(data) / "notepad-plus-plus" / "plugins");
	else if (const char *home = std::getenv("HOME"))
		directories.emplace_back(std::filesystem::path(home) / ".local" / "share" / "notepad-plus-plus" / "plugins");
	std::error_code error;
	const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
	if (!error)
		directories.push_back(executable.parent_path().parent_path() / "lib" / "notepad-plus-plus-linux" / "plugins");
	directories.emplace_back(NPP_LINUX_PLUGIN_DIR);
	return directories;
}

void PluginManager::scan(const std::vector<std::filesystem::path> &directories, const std::vector<std::string> &enabledIds)
{
	std::set<std::string> known;
	std::set<std::filesystem::path> seenDirectories;
	for (const auto &plugin : _plugins)
		known.insert(plugin.id);
	for (const auto &directory : directories)
	{
		std::error_code error;
		const auto canonical = std::filesystem::weakly_canonical(directory, error);
		if (error || !std::filesystem::is_directory(canonical, error) || !seenDirectories.insert(canonical).second)
			continue;
		std::vector<std::filesystem::path> files;
		for (std::filesystem::directory_iterator iterator(canonical, error); !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error))
		{
			const auto &path = iterator->path();
			if (iterator->is_directory(error))
				addCandidate(files, path / (path.filename().string() + ".so"));
			else
				addCandidate(files, path);
		}
		std::sort(files.begin(), files.end());
		for (const auto &file : files)
		{
			const std::string id = file.filename().string();
			if (!known.insert(id).second)
				continue;
			Plugin plugin;
			plugin.file = file;
			plugin.id = id;
			plugin.name = file.stem().string();
			plugin.enabled = std::find(enabledIds.begin(), enabledIds.end(), id) != enabledIds.end();
			_plugins.push_back(std::move(plugin));
		}
	}
}

void PluginManager::loadEnabled(const NppLinuxHost *host)
{
	for (auto &plugin : _plugins)
	{
		if (plugin.enabled && !plugin.loaded && plugin.error.empty())
			load(plugin, host);
	}
}

bool PluginManager::load(Plugin &plugin, const NppLinuxHost *host)
{
	if (plugin.loaded)
		return true;
	plugin.error.clear();
	struct stat information {};
	if (::stat(plugin.file.c_str(), &information) != 0)
	{
		plugin.error = "arquivo não encontrado";
		return false;
	}
	if (information.st_mode & S_IWOTH)
	{
		plugin.error = "permissões inseguras: qualquer usuário pode alterar o arquivo";
		return false;
	}
	void *handle = ::dlopen(plugin.file.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle)
	{
		const char *message = ::dlerror();
		plugin.error = message ? message : "não foi possível carregar a biblioteca";
		return false;
	}
	auto infoFunction = reinterpret_cast<NppLinuxPluginInfoFunction>(::dlsym(handle, NPP_LINUX_PLUGIN_INFO_SYMBOL));
	const NppLinuxPluginInfo *info = infoFunction ? infoFunction() : nullptr;
	if (!info)
		plugin.error = "a biblioteca não é um plugin do Notepad++ Linux (falta npp_linux_plugin_info)";
	else if (info->apiVersion != NPP_LINUX_PLUGIN_API_VERSION)
		plugin.error = "versão de API " + std::to_string(info->apiVersion) + " não suportada (esperada " + std::to_string(NPP_LINUX_PLUGIN_API_VERSION) + ")";
	else if (!info->name || !*info->name || info->commandCount < 0 || info->commandCount > maxCommands || (info->commandCount > 0 && !info->commands))
		plugin.error = "informações do plugin inválidas";
	if (!plugin.error.empty())
	{
		::dlclose(handle);
		return false;
	}
	auto loadFunction = reinterpret_cast<NppLinuxPluginLoadFunction>(::dlsym(handle, NPP_LINUX_PLUGIN_LOAD_SYMBOL));
	if (loadFunction && !loadFunction(host))
	{
		plugin.error = "o plugin recusou a inicialização";
		::dlclose(handle);
		return false;
	}
	plugin.handle = handle;
	plugin.loaded = true;
	plugin.name = info->name;
	plugin.version = info->version ? info->version : "";
	plugin.description = info->description ? info->description : "";
	plugin.commands.clear();
	for (int i = 0; i < info->commandCount; ++i)
	{
		if (info->commands[i].name && info->commands[i].run)
			plugin.commands.push_back({info->commands[i].name, info->commands[i].run});
	}
	return true;
}

void PluginManager::unloadAll()
{
	for (auto &plugin : _plugins)
	{
		if (!plugin.loaded)
			continue;
		if (auto unload = reinterpret_cast<NppLinuxPluginUnloadFunction>(::dlsym(plugin.handle, NPP_LINUX_PLUGIN_UNLOAD_SYMBOL)))
			unload();
		::dlclose(plugin.handle);
		plugin.handle = nullptr;
		plugin.loaded = false;
		plugin.commands.clear();
	}
}
}
