#ifndef NPP_LINUX_TEXT_TOOLS_H
#define NPP_LINUX_TEXT_TOOLS_H

#include <map>
#include <string>
#include <utility>
#include <vector>

// Pure text transformations used by the Edit, Encoding and Tools menus.
// They work on UTF-8 strings and do not depend on GTK or Scintilla, so the
// self-test can check them directly.
namespace NppLinux
{
enum class SortKind
{
	Lexical,
	LexicalIgnoreCase,
	Locale,
	Integer,
	DecimalComma,
	DecimalDot,
	Length
};

// Sorts lines; with the numeric kinds, lines that are not numbers keep their
// order and stay before the numbers (after them when descending).
void sortLineList(std::vector<std::string> &lines, SortKind kind, bool descending);
std::size_t removeConsecutiveDuplicates(std::vector<std::string> &lines);
std::size_t removeEmptyLines(std::vector<std::string> &lines, bool blankToo);
void shuffleLines(std::vector<std::string> &lines);

enum class CaseKind
{
	Upper,
	Lower,
	Proper,
	ProperBlend,
	Sentence,
	SentenceBlend,
	Invert,
	Random
};
std::string convertCase(const std::string &text, CaseKind kind);

std::string trimLeading(const std::string &line);
std::string trimTrailing(const std::string &line);
std::string tabsToSpaces(const std::string &line, int tabWidth);
std::string spacesToTabs(const std::string &line, int tabWidth, bool leadingOnly);

enum class HashKind
{
	Md5,
	Sha1,
	Sha256,
	Sha512
};
const char *hashName(HashKind kind) noexcept;
std::string hashHex(HashKind kind, const std::string &data);

// Replaces $(NAME) with values; unknown names are left untouched.
std::string expandVariables(const std::string &text, const std::map<std::string, std::string> &values);
// Quotes a value for /bin/sh.
std::string shellQuote(const std::string &value);

struct EncodingInfo
{
	const char *id;
	const char *label;
	const char *group;
};
// Encodings offered in the Encoding menu. Ids are stored in the session.
const std::vector<EncodingInfo> &encodings();
std::string encodingLabel(const std::string &id);
// Reads file bytes: detects UTF-8/UTF-16 BOMs and valid UTF-8, otherwise
// falls back to Windows-1252 (a superset of ISO-8859-1 for printable text).
// With a forced encoding the bytes are read as that encoding.
bool decodeBytes(const std::string &bytes, const std::string &forced, std::string &encoding, std::string &utf8, std::string &error);
// Writes UTF-8 text in the given encoding, with a BOM when it has one.
bool encodeText(const std::string &utf8, const std::string &encoding, std::string &bytes, std::string &error);

// Block comment delimiters for a built-in language id ({"",""} when none).
std::pair<std::string, std::string> blockCommentFor(const std::string &languageId);
}

#endif
