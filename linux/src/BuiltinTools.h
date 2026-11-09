#ifndef NPP_LINUX_BUILTIN_TOOLS_H
#define NPP_LINUX_BUILTIN_TOOLS_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Native versions of the features of popular Windows plugins (ComparePlus,
// MIME Tools, NppConverter, JSMin). Pure text functions, tested directly.
namespace NppLinux
{
std::string base64Encode(const std::string &data);
bool base64Decode(const std::string &text, std::string &data);
std::string urlEncode(const std::string &text);
bool urlDecode(const std::string &text, std::string &decoded);
// "Olá" -> "4F6CC3A1"; hexToAscii accepts spaces and an optional 0x.
std::string asciiToHex(const std::string &text);
bool hexToAscii(const std::string &hex, std::string &text);

// JavaScript minifier following Douglas Crockford's JSMin.
bool jsMinify(const std::string &source, std::string &result, std::string &error);
// Simple re-indenting formatter: a line break after { ; } and indentation by
// nesting. Strings and comments are kept as they are.
std::string jsFormat(const std::string &source, const std::string &indent);

struct DiffLine
{
	char kind = ' '; // ' ' same, '-' only in the first text, '+' only in the second
	std::size_t left = 0;  // 0-based line in the first text (for ' ' and '-')
	std::size_t right = 0; // 0-based line in the second text (for ' ' and '+')
	std::string text;
};
// Search mode "Extended" of the Find dialog: \n \r \t \0 \\ \xHH \uHHHH
// \oOOO \dDDD and \bBBBBBBBB become the characters they name.
bool expandExtended(const std::string &text, std::string &result);
std::vector<std::string> splitLines(const std::string &text);
std::vector<DiffLine> diffLines(const std::vector<std::string> &left, const std::vector<std::string> &right);
// Unified diff with three lines of context; empty when the texts are equal.
std::string unifiedDiff(const std::vector<DiffLine> &diff, const std::string &leftName, const std::string &rightName);

// Side-by-side comparison: one row per screen line of both panes.
struct CompareRow
{
	char kind = ' '; // ' ' same, '~' changed (both sides), '-' only left, '+' only right
	long left = -1;  // 0-based line of the first text, -1 for an empty filler row
	long right = -1; // 0-based line of the second text, -1 for an empty filler row
};
// Removed and added lines of the same change are paired on the same row, as in
// the split view of GitHub; the rest is aligned with empty filler rows.
std::vector<CompareRow> alignDiff(const std::vector<DiffLine> &diff);
// Line used only to decide whether two lines are equal.
std::string compareKey(const std::string &line, bool ignoreSpaces, bool ignoreCase);
// Parts (byte offset, length) of two changed lines that differ, word by word.
// Both lists are empty when the lines have nothing in common.
struct WordChanges
{
	std::vector<std::pair<std::size_t, std::size_t>> left;
	std::vector<std::pair<std::size_t, std::size_t>> right;
};
WordChanges wordDiff(const std::string &left, const std::string &right);
}

#endif
