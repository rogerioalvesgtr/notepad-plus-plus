#ifndef NPP_LINUX_USER_LANGUAGE_H
#define NPP_LINUX_USER_LANGUAGE_H

#include "Theme.h"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace NppLinux
{
class EditorDocument;

// Style numbers used by the built-in user-language highlighter.
enum UserStyle
{
	UserDefault = 0,
	UserComment = 1,
	UserLineComment = 2,
	UserNumber = 3,
	UserKeyword1 = 4,  // ... UserKeyword1 + 7
	UserOperator = 12,
	UserDelimiter1 = 13 // ... UserDelimiter1 + 7
};

struct UserDelimiter
{
	std::string open;
	std::string escape;
	std::string close; // "((EOL))" closes at the end of the line
};

// A user-defined language. Files use the Notepad++ "UserLang" XML format, so
// definitions exported from Notepad++ for Windows can be imported here.
struct UserLanguage
{
	std::string name;
	std::vector<std::string> extensions;
	bool caseIgnored = false;
	std::vector<std::string> lineComments;
	std::string blockOpen;
	std::string blockClose;
	std::array<std::vector<std::string>, 8> keywords;
	std::vector<std::string> operators;
	std::array<UserDelimiter, 8> delimiters;
	std::map<int, StyleSpec> styles; // colours stored in the file, by UserStyle
	std::filesystem::path file;
};

bool parseUserLanguages(const std::string &xml, std::vector<UserLanguage> &languages, std::string &error);
std::string userLanguageToXml(const UserLanguage &language);
std::vector<UserLanguage> loadUserLanguages(const std::filesystem::path &directory, std::vector<std::string> &errors);
bool saveUserLanguage(const std::filesystem::path &directory, UserLanguage &language, std::string &error);
std::string userLanguageFileName(const std::string &name);
std::vector<std::string> splitWords(const std::string &text);
std::string joinWords(const std::vector<std::string> &words);

// Styles the range from the line containing start up to end (container lexing).
void styleUserLanguage(EditorDocument &document, const UserLanguage &language, long start, long end);
}

#endif
