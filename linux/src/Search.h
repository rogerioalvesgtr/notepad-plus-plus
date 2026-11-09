#ifndef NPP_LINUX_SEARCH_H
#define NPP_LINUX_SEARCH_H

#include "EditorDocument.h"

#include <cstddef>
#include <string>

namespace NppLinux
{
struct SearchOptions
{
	bool matchCase = false;
	bool wholeWord = false;
	bool regularExpression = false;
	bool wrapAround = true;
};

struct SearchResult
{
	bool found = false;
	bool wrapped = false;
	std::size_t count = 0;
};

SearchResult find(EditorDocument &document, const std::string &needle,
	const SearchOptions &options, bool backwards = false);
std::size_t replaceCurrent(EditorDocument &document, const std::string &needle,
	const std::string &replacement, const SearchOptions &options);
std::size_t replaceAll(EditorDocument &document, const std::string &needle,
	const std::string &replacement, const SearchOptions &options);
std::size_t countMatches(EditorDocument &document, const std::string &needle,
	const SearchOptions &options);
bool goToLine(EditorDocument &document, long line);
bool validateRegularExpression(const std::string &expression);
int searchFlags(const SearchOptions &options);
}

#endif
