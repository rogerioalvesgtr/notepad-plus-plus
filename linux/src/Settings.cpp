#include "Settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <glib.h>

namespace NppLinux
{
namespace
{
std::filesystem::path configDirectory()
{
	if (const char *xdgConfig = std::getenv("XDG_CONFIG_HOME"))
		return std::filesystem::path(xdgConfig) / "notepad-plus-plus";
	if (const char *home = std::getenv("HOME"))
		return std::filesystem::path(home) / ".config" / "notepad-plus-plus";
	return std::filesystem::current_path() / ".notepad-plus-plus";
}

bool readBool(const std::string &xml, const std::string &name, bool fallback)
{
	const std::string tag = "<" + name + ">";
	const auto begin = xml.find(tag);
	if (begin == std::string::npos)
		return fallback;
	const auto value = xml.substr(begin + tag.size(), xml.find("</", begin) - begin - tag.size());
	return value == "true" || value == "1";
}

int readInt(const std::string &xml, const std::string &name, int fallback, int minimum, int maximum)
{
	const std::string tag = "<" + name + ">";
	const auto begin = xml.find(tag);
	if (begin == std::string::npos)
		return fallback;
	try
	{
		return std::clamp(std::stoi(xml.substr(begin + tag.size())), minimum, maximum);
	}
	catch (...)
	{
		return fallback;
	}
}

std::string readString(const std::string &xml, const std::string &name, const std::string &fallback);
}

std::string unescapeXml(std::string text)
{
	const std::pair<std::string, std::string> entities[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
	for (const auto &[entity, character] : entities)
	{
		std::size_t position = 0;
		while ((position = text.find(entity, position)) != std::string::npos)
		{
			text.replace(position, entity.size(), character);
			position += character.size();
		}
	}
	return text;
}

SettingsStore::SettingsStore(bool ephemeral)
	: _path(ephemeral ? std::filesystem::temp_directory_path() / ("notepad-plus-plus-linux-test-" + std::to_string(static_cast<long>(g_get_real_time())) ) / "settings.xml" : configDirectory() / "settings.xml"), _ephemeral(ephemeral)
{
}

const std::filesystem::path &SettingsStore::path() const noexcept
{
	return _path;
}

std::string escapeXml(std::string_view text)
{
	std::string result;
	result.reserve(text.size());
	for (const char c : text)
	{
		switch (c)
		{
			case '&': result += "&amp;"; break;
			case '<': result += "&lt;"; break;
			case '>': result += "&gt;"; break;
			case '"': result += "&quot;"; break;
			case '\'': result += "&apos;"; break;
			default: result += c;
		}
	}
	return result;
}

Settings SettingsStore::load() const
{
	Settings settings;
	std::ifstream input(_path, std::ios::binary);
	if (!input)
		return settings;
	std::ostringstream buffer;
	buffer << input.rdbuf();
	const std::string xml = buffer.str();
	settings.darkTheme = readBool(xml, "darkTheme", settings.darkTheme);
	settings.wordWrap = readBool(xml, "wordWrap", settings.wordWrap);
	settings.showWhitespace = readBool(xml, "showWhitespace", settings.showWhitespace);
	settings.showEndOfLine = readBool(xml, "showEndOfLine", settings.showEndOfLine);
	settings.lineNumbers = readBool(xml, "lineNumbers", settings.lineNumbers);
	settings.restoreSession = readBool(xml, "restoreSession", settings.restoreSession);
	settings.useTabs = readBool(xml, "useTabs", settings.useTabs);
	settings.tabWidth = readInt(xml, "tabWidth", settings.tabWidth, 1, 16);
	settings.fontSize = readInt(xml, "fontSize", settings.fontSize, 6, 72);
	const std::string fontTag = "<fontFamily>";
	if (const auto position = xml.find(fontTag); position != std::string::npos)
		settings.fontFamily = unescapeXml(xml.substr(position + fontTag.size(), xml.find("</fontFamily>", position) - position - fontTag.size()));
	const std::string languageTag = "<lastLanguage>";
	if (const auto position = xml.find(languageTag); position != std::string::npos)
		settings.lastLanguage = unescapeXml(xml.substr(position + languageTag.size(), xml.find("</lastLanguage>", position) - position - languageTag.size()));
	settings.findFolder = readString(xml, "findFolder", settings.findFolder);
	settings.findFilters = readString(xml, "findFilters", settings.findFilters);
	settings.findRecursive = readBool(xml, "findRecursive", settings.findRecursive);
	settings.findHidden = readBool(xml, "findHidden", settings.findHidden);
	settings.showWorkspace = readBool(xml, "showWorkspace", settings.showWorkspace);
	settings.showFunctionList = readBool(xml, "showFunctionList", settings.showFunctionList);
	settings.showDocumentMap = readBool(xml, "showDocumentMap", settings.showDocumentMap);
	settings.functionListSorted = readBool(xml, "functionListSorted", settings.functionListSorted);
	settings.autoIndent = readBool(xml, "autoIndent", settings.autoIndent);
	settings.highlightCaretLine = readBool(xml, "highlightCaretLine", settings.highlightCaretLine);
	settings.braceMatching = readBool(xml, "braceMatching", settings.braceMatching);
	settings.indentGuides = readBool(xml, "indentGuides", settings.indentGuides);
	settings.autoCloseBrackets = readBool(xml, "autoCloseBrackets", settings.autoCloseBrackets);
	settings.edgeColumn = readInt(xml, "edgeColumn", settings.edgeColumn, 0, 500);
	settings.backupEnabled = readBool(xml, "backupEnabled", settings.backupEnabled);
	settings.backupInterval = readInt(xml, "backupInterval", settings.backupInterval, 5, 3600);
	settings.defaultEolWindows = readBool(xml, "defaultEolWindows", settings.defaultEolWindows);
	settings.defaultLanguage = readString(xml, "defaultLanguage", settings.defaultLanguage);
	settings.maxRecentFiles = readInt(xml, "maxRecentFiles", settings.maxRecentFiles, 0, 30);
	settings.lightStyles = readString(xml, "lightStyles", settings.lightStyles);
	settings.darkStyles = readString(xml, "darkStyles", settings.darkStyles);
	settings.rememberUnsaved = readBool(xml, "rememberUnsaved", settings.rememberUnsaved);
	// As in the Windows version, keeping unsaved text on exit also keeps the
	// session: one without the other would lose the text.
	if (settings.rememberUnsaved)
		settings.restoreSession = true;
	settings.showWrapSymbol = readBool(xml, "showWrapSymbol", settings.showWrapSymbol);
	settings.showBookmarkMargin = readBool(xml, "showBookmarkMargin", settings.showBookmarkMargin);
	settings.showFoldMargin = readBool(xml, "showFoldMargin", settings.showFoldMargin);
	settings.changeHistory = readBool(xml, "changeHistory", settings.changeHistory);
	settings.newDocumentEncoding = readString(xml, "newDocumentEncoding", settings.newDocumentEncoding);
	settings.dateTimeFormat = readString(xml, "dateTimeFormat", settings.dateTimeFormat);
	settings.searchEngine = readString(xml, "searchEngine", settings.searchEngine);
	settings.lastRunCommand = readString(xml, "lastRunCommand", settings.lastRunCommand);
	settings.fileAutoDetect = readBool(xml, "fileAutoDetect", settings.fileAutoDetect);
	settings.autoReloadSilently = readBool(xml, "autoReloadSilently", settings.autoReloadSilently);
	settings.nppLanguageColors = readBool(xml, "nppLanguageColors", settings.nppLanguageColors);
	for (std::size_t plugin = 0; (plugin = xml.find("<enabledPlugin>", plugin)) != std::string::npos;)
	{
		const auto end = xml.find("</enabledPlugin>", plugin);
		if (end == std::string::npos)
			break;
		settings.enabledPlugins.push_back(unescapeXml(xml.substr(plugin + 15, end - plugin - 15)));
		plugin = end + 16;
	}
	for (std::size_t folder = 0; (folder = xml.find("<workspaceFolder>", folder)) != std::string::npos;)
	{
		const auto end = xml.find("</workspaceFolder>", folder);
		if (end == std::string::npos)
			break;
		settings.workspaceFolders.push_back(unescapeXml(xml.substr(folder + 17, end - folder - 17)));
		folder = end + 18;
	}
	std::size_t position = 0;
	while ((position = xml.find("<recentFile>", position)) != std::string::npos)
	{
		const auto end = xml.find("</recentFile>", position);
		if (end == std::string::npos)
			break;
		settings.recentFiles.push_back(unescapeXml(xml.substr(position + 12, end - position - 12)));
		position = end + 13;
	}
	return settings;
}

bool SettingsStore::save(const Settings &settings) const
{
	std::error_code error;
	std::filesystem::create_directories(_path.parent_path(), error);
	if (error)
		return false;
	const std::filesystem::path temporary = _path.string() + ".tmp";
	std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
	if (!output)
		return false;
	auto boolean = [](bool value) { return value ? "true" : "false"; };
	output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<NotepadPlusPlusLinux version=\"1\">\n";
	output << "\t<darkTheme>" << boolean(settings.darkTheme) << "</darkTheme>\n";
	output << "\t<wordWrap>" << boolean(settings.wordWrap) << "</wordWrap>\n";
	output << "\t<showWhitespace>" << boolean(settings.showWhitespace) << "</showWhitespace>\n";
	output << "\t<showEndOfLine>" << boolean(settings.showEndOfLine) << "</showEndOfLine>\n";
	output << "\t<lineNumbers>" << boolean(settings.lineNumbers) << "</lineNumbers>\n";
	output << "\t<restoreSession>" << boolean(settings.restoreSession) << "</restoreSession>\n";
	output << "\t<useTabs>" << boolean(settings.useTabs) << "</useTabs>\n";
	output << "\t<tabWidth>" << settings.tabWidth << "</tabWidth>\n";
	output << "\t<fontSize>" << settings.fontSize << "</fontSize>\n";
	output << "\t<fontFamily>" << escapeXml(settings.fontFamily) << "</fontFamily>\n";
	output << "\t<lastLanguage>" << escapeXml(settings.lastLanguage) << "</lastLanguage>\n";
	output << "\t<findFolder>" << escapeXml(settings.findFolder) << "</findFolder>\n";
	output << "\t<findFilters>" << escapeXml(settings.findFilters) << "</findFilters>\n";
	output << "\t<findRecursive>" << boolean(settings.findRecursive) << "</findRecursive>\n";
	output << "\t<findHidden>" << boolean(settings.findHidden) << "</findHidden>\n";
	output << "\t<showWorkspace>" << boolean(settings.showWorkspace) << "</showWorkspace>\n";
	output << "\t<showFunctionList>" << boolean(settings.showFunctionList) << "</showFunctionList>\n";
	output << "\t<showDocumentMap>" << boolean(settings.showDocumentMap) << "</showDocumentMap>\n";
	output << "\t<functionListSorted>" << boolean(settings.functionListSorted) << "</functionListSorted>\n";
	for (const auto &folder : settings.workspaceFolders)
		output << "\t<workspaceFolder>" << escapeXml(folder) << "</workspaceFolder>\n";
	output << "\t<autoIndent>" << boolean(settings.autoIndent) << "</autoIndent>\n";
	output << "\t<highlightCaretLine>" << boolean(settings.highlightCaretLine) << "</highlightCaretLine>\n";
	output << "\t<braceMatching>" << boolean(settings.braceMatching) << "</braceMatching>\n";
	output << "\t<indentGuides>" << boolean(settings.indentGuides) << "</indentGuides>\n";
	output << "\t<autoCloseBrackets>" << boolean(settings.autoCloseBrackets) << "</autoCloseBrackets>\n";
	output << "\t<edgeColumn>" << settings.edgeColumn << "</edgeColumn>\n";
	output << "\t<backupEnabled>" << boolean(settings.backupEnabled) << "</backupEnabled>\n";
	output << "\t<backupInterval>" << settings.backupInterval << "</backupInterval>\n";
	output << "\t<defaultEolWindows>" << boolean(settings.defaultEolWindows) << "</defaultEolWindows>\n";
	output << "\t<defaultLanguage>" << escapeXml(settings.defaultLanguage) << "</defaultLanguage>\n";
	output << "\t<maxRecentFiles>" << settings.maxRecentFiles << "</maxRecentFiles>\n";
	output << "\t<lightStyles>" << escapeXml(settings.lightStyles) << "</lightStyles>\n";
	output << "\t<darkStyles>" << escapeXml(settings.darkStyles) << "</darkStyles>\n";
	output << "\t<rememberUnsaved>" << boolean(settings.rememberUnsaved) << "</rememberUnsaved>\n";
	output << "\t<showWrapSymbol>" << boolean(settings.showWrapSymbol) << "</showWrapSymbol>\n";
	output << "\t<showBookmarkMargin>" << boolean(settings.showBookmarkMargin) << "</showBookmarkMargin>\n";
	output << "\t<showFoldMargin>" << boolean(settings.showFoldMargin) << "</showFoldMargin>\n";
	output << "\t<changeHistory>" << boolean(settings.changeHistory) << "</changeHistory>\n";
	output << "\t<newDocumentEncoding>" << escapeXml(settings.newDocumentEncoding) << "</newDocumentEncoding>\n";
	output << "\t<dateTimeFormat>" << escapeXml(settings.dateTimeFormat) << "</dateTimeFormat>\n";
	output << "\t<searchEngine>" << escapeXml(settings.searchEngine) << "</searchEngine>\n";
	output << "\t<lastRunCommand>" << escapeXml(settings.lastRunCommand) << "</lastRunCommand>\n";
	output << "\t<fileAutoDetect>" << boolean(settings.fileAutoDetect) << "</fileAutoDetect>\n";
	output << "\t<autoReloadSilently>" << boolean(settings.autoReloadSilently) << "</autoReloadSilently>\n";
	output << "\t<nppLanguageColors>" << boolean(settings.nppLanguageColors) << "</nppLanguageColors>\n";
	for (const auto &plugin : settings.enabledPlugins)
		output << "\t<enabledPlugin>" << escapeXml(plugin) << "</enabledPlugin>\n";
	for (const auto &file : settings.recentFiles)
		output << "\t<recentFile>" << escapeXml(file) << "</recentFile>\n";
	output << "</NotepadPlusPlusLinux>\n";
	output.close();
	if (!output)
	{
		std::filesystem::remove(temporary, error);
		return false;
	}
	std::filesystem::rename(temporary, _path, error);
	return !error;
}

namespace
{
std::string readString(const std::string &xml, const std::string &name, const std::string &fallback)
{
	const std::string open = "<" + name + ">";
	const std::string close = "</" + name + ">";
	const auto begin = xml.find(open);
	if (begin == std::string::npos)
		return fallback;
	const auto end = xml.find(close, begin);
	if (end == std::string::npos)
		return fallback;
	return unescapeXml(xml.substr(begin + open.size(), end - begin - open.size()));
}
}

void rememberRecentFile(Settings &settings, const std::filesystem::path &path)
{
	const std::string normal = std::filesystem::absolute(path).lexically_normal().string();
	settings.recentFiles.erase(std::remove(settings.recentFiles.begin(), settings.recentFiles.end(), normal), settings.recentFiles.end());
	settings.recentFiles.insert(settings.recentFiles.begin(), normal);
	if (settings.recentFiles.size() > static_cast<std::size_t>(std::max(settings.maxRecentFiles, 0)))
		settings.recentFiles.resize(static_cast<std::size_t>(std::max(settings.maxRecentFiles, 0)));
}
}
