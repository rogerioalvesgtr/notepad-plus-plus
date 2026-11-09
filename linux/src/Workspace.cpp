#include "Workspace.h"

#include "Settings.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include <glib.h>

namespace NppLinux
{
namespace
{
std::string sortKey(const std::string &name)
{
	if (!g_utf8_validate(name.c_str(), static_cast<gssize>(name.size()), nullptr))
		return name;
	// Case-insensitive even when the C locale is active (for example under xvfb).
	gchar *folded = g_utf8_casefold(name.c_str(), static_cast<gssize>(name.size()));
	gchar *collated = g_utf8_collate_key_for_filename(folded ? folded : name.c_str(), -1);
	std::string key = collated ? collated : name;
	g_free(collated);
	g_free(folded);
	return key;
}
}

std::vector<DirectoryEntry> listDirectory(const std::filesystem::path &folder, bool includeHidden, std::string &error)
{
	std::vector<DirectoryEntry> entries;
	std::error_code code;
	std::filesystem::directory_iterator iterator(folder, std::filesystem::directory_options::skip_permission_denied, code);
	if (code)
	{
		error = code.message();
		return entries;
	}
	for (; iterator != std::filesystem::directory_iterator(); iterator.increment(code))
	{
		if (code)
		{
			error = code.message();
			break;
		}
		const std::string name = iterator->path().filename().string();
		if (!includeHidden && !name.empty() && name.front() == '.')
			continue;
		std::error_code typeError;
		const bool directory = iterator->is_directory(typeError);
		if (!directory && !iterator->is_regular_file(typeError))
			continue;
		entries.push_back({name, iterator->path(), directory});
	}
	std::vector<std::pair<std::string, std::size_t>> keys;
	keys.reserve(entries.size());
	for (std::size_t i = 0; i < entries.size(); ++i)
		keys.emplace_back(sortKey(entries[i].name), i);
	std::sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) {
		if (entries[a.second].directory != entries[b.second].directory)
			return entries[a.second].directory;
		return a.first < b.first;
	});
	std::vector<DirectoryEntry> sorted;
	sorted.reserve(entries.size());
	for (const auto &key : keys)
		sorted.push_back(std::move(entries[key.second]));
	return sorted;
}

bool saveWorkspaceFile(const std::filesystem::path &file, const std::vector<std::string> &folders, std::string &error)
{
	const std::filesystem::path temporary = file.string() + ".tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			error = "Não foi possível gravar " + file.string();
			return false;
		}
		output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<NotepadPlusPlusLinuxWorkspace version=\"1\">\n";
		for (const auto &folder : folders)
			output << "\t<folder path=\"" << escapeXml(folder) << "\"/>\n";
		output << "</NotepadPlusPlusLinuxWorkspace>\n";
		output.close();
		if (!output)
		{
			error = "Não foi possível gravar " + file.string();
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			return false;
		}
	}
	std::error_code code;
	std::filesystem::rename(temporary, file, code);
	if (code)
	{
		error = code.message();
		std::filesystem::remove(temporary, code);
		return false;
	}
	return true;
}

std::vector<std::string> loadWorkspaceFile(const std::filesystem::path &file, std::string &error)
{
	std::vector<std::string> folders;
	std::ifstream input(file, std::ios::binary);
	if (!input)
	{
		error = "Não foi possível abrir " + file.string();
		return folders;
	}
	std::ostringstream buffer;
	buffer << input.rdbuf();
	const std::string xml = buffer.str();
	if (xml.find("<NotepadPlusPlusLinuxWorkspace") == std::string::npos)
	{
		error = "O arquivo não é um espaço de trabalho do Notepad++ Linux.";
		return folders;
	}
	const std::string marker = "<folder path=\"";
	std::size_t position = 0;
	while ((position = xml.find(marker, position)) != std::string::npos)
	{
		position += marker.size();
		const auto end = xml.find('"', position);
		if (end == std::string::npos)
			break;
		const std::string folder = unescapeXml(xml.substr(position, end - position));
		if (!folder.empty() && std::find(folders.begin(), folders.end(), folder) == folders.end())
			folders.push_back(folder);
		position = end + 1;
	}
	return folders;
}
}
