#ifndef NPP_LINUX_FUNCTION_LIST_H
#define NPP_LINUX_FUNCTION_LIST_H

#include <cstddef>
#include <string>
#include <vector>

namespace NppLinux
{
struct FunctionEntry
{
	std::string name;
	std::string kind;   // "função", "classe", "namespace", "título"...
	std::string parent; // enclosing class or namespace, when known
	bool container = false;
	std::size_t line = 0;     // 1-based
	std::size_t position = 0; // byte offset of the name in the UTF-8 text
};

// True when the language has a function-list parser.
bool functionListSupports(const std::string &languageId);

// Lists functions, classes and similar landmarks. Comments and string literals
// are ignored. The parser is heuristic and does not compile the code.
std::vector<FunctionEntry> parseFunctions(const std::string &languageId, const std::string &text);

// Replaces comments and string literals by spaces, keeping offsets and line breaks.
std::string blankCommentsAndStrings(const std::string &languageId, const std::string &text);
}

#endif
