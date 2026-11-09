// Native Linux editor for Notepad++.
// This file is independent of the Win32 application.

#ifndef NPP_LINUX_LANGUAGE_SUPPORT_H
#define NPP_LINUX_LANGUAGE_SUPPORT_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace NppLinux
{
struct Language
{
	std::string id;
	std::string name;
	std::string lexer;
	std::vector<std::string> extensions;
	std::string keywords;
	std::vector<int> keywordStyles;
	std::vector<int> commentStyles;
	std::vector<int> stringStyles;
	std::vector<int> numberStyles;
	std::vector<int> operatorStyles;
};

const std::vector<Language> &languages();
const Language &languageForPath(const std::filesystem::path &path);
const Language &plainTextLanguage();
std::string normaliseUtf8(const std::string &bytes, std::string &encoding);
std::string trimBom(std::string bytes);
}

#endif
