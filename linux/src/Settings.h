#ifndef NPP_LINUX_SETTINGS_H
#define NPP_LINUX_SETTINGS_H

#include <filesystem>
#include <string>
#include <vector>

namespace NppLinux
{
struct Settings
{
	bool darkTheme = false;
	bool wordWrap = true;
	bool showWhitespace = false;
	bool showEndOfLine = false;
	bool lineNumbers = true;
	bool restoreSession = true;
	bool useTabs = false;
	int tabWidth = 4;
	int fontSize = 11;
	std::string fontFamily = "Monospace";
	std::string lastLanguage;
	std::vector<std::string> recentFiles;
	std::string findFolder;
	std::string findFilters = "*";
	bool findRecursive = true;
	bool findHidden = false;
	bool showWorkspace = false;
	bool showFunctionList = false;
	bool showDocumentMap = false;
	bool functionListSorted = false;
	std::vector<std::string> workspaceFolders;
	bool autoIndent = true;
	bool highlightCaretLine = true;
	bool braceMatching = true;
	bool indentGuides = false;
	bool autoCloseBrackets = false;
	int edgeColumn = 0;
	bool backupEnabled = true;
	int backupInterval = 30;
	bool defaultEolWindows = false;
	std::string defaultLanguage;
	int maxRecentFiles = 10;
	std::string lightStyles;
	std::string darkStyles;
	std::vector<std::string> enabledPlugins;
	// Windows-like session snapshot: quitting keeps unsaved text without asking.
	bool rememberUnsaved = true;
	bool showWrapSymbol = false;
	bool showBookmarkMargin = true;
	bool showFoldMargin = true;
	bool changeHistory = true;
	std::string newDocumentEncoding = "UTF-8";
	std::string dateTimeFormat = "%Y-%m-%d %H:%M:%S";
	std::string searchEngine = "https://duckduckgo.com/?q=$(TEXT)";
	std::string lastRunCommand;
	// Notice files changed by other programs (Windows: File Status Auto-Detection).
	bool fileAutoDetect = true;
	bool autoReloadSilently = false;
	// Colours per language from the Windows version (keywords, types, numbers...).
	bool nppLanguageColors = true;
};

class SettingsStore
{
public:
	explicit SettingsStore(bool ephemeral = false);
	Settings load() const;
	bool save(const Settings &settings) const;
	const std::filesystem::path &path() const noexcept;

private:
	std::filesystem::path _path;
	bool _ephemeral = false;
};

void rememberRecentFile(Settings &settings, const std::filesystem::path &path);
std::string escapeXml(std::string_view text);
std::string unescapeXml(std::string text);
}

#endif
