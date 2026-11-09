#ifndef NPP_LINUX_SESSION_STORE_H
#define NPP_LINUX_SESSION_STORE_H

#include "EditorDocument.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace NppLinux
{
struct MacroCommand
{
	unsigned int message = 0;
	uptr_t wParam = 0;
	sptr_t lParam = 0;
	bool hasText = false;
	std::string text;
};

struct SessionEntry
{
	std::string name;
	std::string path;
	std::string backup;
	std::string language;
	std::string encoding;
	int eol = 2;
	bool modified = false;
};

class SessionStore
{
public:
	explicit SessionStore(const std::filesystem::path &configDirectory);
	std::filesystem::path backupPath(const EditorDocument &document) const;
	bool backup(const EditorDocument &document);
	void discardBackup(const EditorDocument &document);
	std::vector<SessionEntry> load() const;
	bool save(const std::vector<std::unique_ptr<EditorDocument>> &documents) const;
	std::vector<MacroCommand> loadMacro() const;
	bool saveMacro(const std::vector<MacroCommand> &commands) const;
	// Named macros live in <config>/macros/<name>.xml.
	std::filesystem::path macroDirectory() const;
	static std::vector<MacroCommand> loadMacroFile(const std::filesystem::path &file);
	static bool saveMacroFile(const std::filesystem::path &file, const std::vector<MacroCommand> &commands);
	// Session files in the Notepad++ XML format (<NotepadPlus><Session><mainView><File filename=.../>).
	static bool saveSessionFile(const std::filesystem::path &file, const std::vector<std::string> &paths);
	static std::vector<std::string> loadSessionFile(const std::filesystem::path &file);
	static bool isAllowed(const MacroCommand &command);

private:
	std::filesystem::path _directory;
};
}

#endif
