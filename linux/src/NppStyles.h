#ifndef NPP_LINUX_NPP_STYLES_H
#define NPP_LINUX_NPP_STYLES_H

#include <string>
#include <utility>
#include <vector>

// The colours of each language as the Windows version defines them
// (PowerEditor/src/stylers.model.xml for the light theme and
// PowerEditor/installer/themes/DarkModeDefault.xml for the dark one), and its
// keyword lists (PowerEditor/src/langs.model.xml). The files are built into
// the program, so nothing has to be installed beside it.
namespace NppLinux
{
struct NppStyle
{
	int id = 0;
	int fore = -1;      // Scintilla colour (0xBBGGRR) or -1
	int back = -1;
	int fontStyle = -1; // bit 1 bold, 2 italic, 4 underline; -1 when not given
	std::string keywordClass;
	std::string extraKeywords;
};

struct NppLexerStyles
{
	std::vector<NppStyle> styles;
	int defaultBack = -1; // the background of the file's Default Style
};

// Which Windows styles and keyword lists a document uses, chosen from the
// built-in language and the file name.
struct NppLanguageSpec
{
	std::string styler;   // LexerType name in the styler files
	std::string language; // Language name in langs.model.xml (may be empty)
	// keyword list name ("instre1", "type1"...) of a language -> Scintilla keyword set
	std::vector<std::pair<std::string, std::pair<std::string, int>>> keywordSets;
	std::vector<std::pair<std::string, std::string>> properties;
};

const NppLexerStyles *nppLexerStyles(const std::string &styler, bool dark);
std::string nppKeywords(const std::string &language, const std::string &list);
// Empty styler when the Windows version has no colours for this language.
NppLanguageSpec nppLanguageSpec(const std::string &languageId, const std::string &fileName);
}

#endif
