#include "FunctionList.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <utility>

#include <glib.h>

namespace NppLinux
{
namespace
{
// ---------------------------------------------------------------- regex helpers

struct RegexMatch
{
	std::vector<std::pair<int, int>> groups;

	bool has(std::size_t index) const
	{
		return index < groups.size() && groups[index].first >= 0;
	}

	std::string text(const std::string &subject, std::size_t index) const
	{
		if (!has(index))
			return {};
		return subject.substr(static_cast<std::size_t>(groups[index].first),
			static_cast<std::size_t>(groups[index].second - groups[index].first));
	}
};

GRegex *regexFor(const char *pattern, int flags)
{
	static std::map<std::pair<std::string, int>, GRegex *> cache;
	const auto key = std::make_pair(std::string(pattern), flags);
	if (const auto found = cache.find(key); found != cache.end())
		return found->second;
	GRegex *regex = g_regex_new(pattern, static_cast<GRegexCompileFlags>(flags | G_REGEX_RAW | G_REGEX_OPTIMIZE),
		static_cast<GRegexMatchFlags>(0), nullptr);
	cache.emplace(key, regex);
	return regex;
}

void searchAll(const char *pattern, int flags, const std::string &subject,
	const std::function<void(const RegexMatch &)> &callback, bool firstOnly = false)
{
	GRegex *regex = regexFor(pattern, flags);
	if (!regex)
		return;
	GMatchInfo *info = nullptr;
	g_regex_match_full(regex, subject.c_str(), static_cast<gssize>(subject.size()), 0,
		static_cast<GRegexMatchFlags>(0), &info, nullptr);
	while (info && g_match_info_matches(info))
	{
		RegexMatch match;
		const int count = g_match_info_get_match_count(info);
		for (int i = 0; i < count; ++i)
		{
			int start = -1;
			int end = -1;
			g_match_info_fetch_pos(info, i, &start, &end);
			match.groups.emplace_back(start, end);
		}
		callback(match);
		if (firstOnly || !g_match_info_next(info, nullptr))
			break;
	}
	g_match_info_free(info);
}

bool searchFirst(const char *pattern, int flags, const std::string &subject, RegexMatch &result)
{
	bool found = false;
	searchAll(pattern, flags, subject, [&](const RegexMatch &match) {
		result = match;
		found = true;
	}, true);
	return found;
}

std::string collapseSpaces(const std::string &text)
{
	std::string result;
	bool space = false;
	for (const char c : text)
	{
		if (g_ascii_isspace(c))
		{
			space = !result.empty();
			continue;
		}
		if (space)
			result += ' ';
		space = false;
		result += c;
	}
	return result;
}

// ---------------------------------------------------------------- cleaning

struct CleanRules
{
	bool slashComments = false;
	bool blockComments = false;
	bool haskellBlocks = false;
	bool hashComments = false;
	bool hashAfterSpaceOnly = false;
	bool dashComments = false;
	bool luaBlocks = false;
	bool semicolonComments = false;
	bool apostropheComments = false;
	bool percentComments = false;
	bool doubleQuoteStrings = false;
	bool singleQuoteStrings = false;
	bool charLiterals = false;
	bool backtickStrings = false;
	bool tripleQuotes = false;
	bool preprocessor = false;
	bool markdownFences = false;
	bool batchRemarks = false;
};

CleanRules rulesFor(const std::string &id)
{
	CleanRules rules;
	if (id == "cpp" || id == "java" || id == "rust" || id == "go")
	{
		rules.slashComments = rules.blockComments = rules.doubleQuoteStrings = rules.charLiterals = true;
		rules.preprocessor = id != "go";
		rules.backtickStrings = id == "go";
	}
	else if (id == "javascript" || id == "dart")
	{
		rules.slashComments = rules.blockComments = rules.doubleQuoteStrings = rules.singleQuoteStrings = true;
		rules.backtickStrings = id == "javascript";
	}
	else if (id == "php")
		rules.slashComments = rules.blockComments = rules.hashComments = rules.doubleQuoteStrings = rules.singleQuoteStrings = true;
	else if (id == "python")
		rules.hashComments = rules.doubleQuoteStrings = rules.singleQuoteStrings = rules.tripleQuotes = true;
	else if (id == "shell" || id == "ruby" || id == "perl" || id == "make" || id == "cmake" || id == "yaml"
		|| id == "dockerfile" || id == "powershell")
		rules.hashComments = rules.hashAfterSpaceOnly = true;
	else if (id == "toml")
		rules.hashComments = rules.hashAfterSpaceOnly = rules.semicolonComments = true;
	else if (id == "nsis")
		rules.hashComments = rules.hashAfterSpaceOnly = rules.semicolonComments = rules.blockComments = true;
	else if (id == "lua")
		rules.dashComments = rules.luaBlocks = rules.doubleQuoteStrings = rules.singleQuoteStrings = true;
	else if (id == "sql")
		rules.dashComments = rules.blockComments = rules.singleQuoteStrings = true;
	else if (id == "vb")
		rules.apostropheComments = rules.doubleQuoteStrings = true;
	else if (id == "tex")
		rules.percentComments = true;
	else if (id == "haskell")
		rules.dashComments = rules.haskellBlocks = rules.doubleQuoteStrings = true;
	else if (id == "css")
		rules.blockComments = rules.slashComments = true;
	else if (id == "markdown")
		rules.markdownFences = true;
	else if (id == "batch")
		rules.batchRemarks = true;
	return rules;
}

void blank(std::string &text, std::size_t from, std::size_t to)
{
	to = std::min(to, text.size());
	for (std::size_t i = from; i < to; ++i)
	{
		if (text[i] != '\n' && text[i] != '\r')
			text[i] = ' ';
	}
}

bool onlySpacesBefore(const std::string &text, std::size_t position)
{
	while (position > 0)
	{
		const char c = text[--position];
		if (c == '\n')
			return true;
		if (c != ' ' && c != '\t')
			return false;
	}
	return true;
}

std::size_t lineEnd(const std::string &text, std::size_t from)
{
	const auto end = text.find('\n', from);
	return end == std::string::npos ? text.size() : end;
}

std::size_t quotedEnd(const std::string &text, std::size_t start, char quote, bool multiline)
{
	std::size_t i = start + 1;
	while (i < text.size() && text[i] != quote && (multiline || text[i] != '\n'))
		i += text[i] == '\\' ? 2 : 1;
	return i < text.size() && text[i] == quote ? i + 1 : std::min(i, text.size());
}

std::string cleanLines(const std::string &text, const std::function<bool(const std::string &line, bool &state)> &blankLine)
{
	std::string result = text;
	bool state = false;
	std::size_t start = 0;
	while (start <= result.size())
	{
		const std::size_t end = lineEnd(result, start);
		if (blankLine(text.substr(start, end - start), state))
			blank(result, start, end);
		if (end >= result.size())
			break;
		start = end + 1;
	}
	return result;
}
}

std::string blankCommentsAndStrings(const std::string &languageId, const std::string &text)
{
	const CleanRules rules = rulesFor(languageId);
	if (rules.markdownFences)
	{
		return cleanLines(text, [](const std::string &line, bool &inFence) {
			const auto first = line.find_first_not_of(" \t");
			const bool fence = first != std::string::npos && (line.compare(first, 3, "```") == 0 || line.compare(first, 3, "~~~") == 0);
			if (fence)
			{
				inFence = !inFence;
				return true;
			}
			return inFence;
		});
	}
	if (rules.batchRemarks)
	{
		return cleanLines(text, [](const std::string &line, bool &) {
			const auto first = line.find_first_not_of(" \t@");
			if (first == std::string::npos)
				return false;
			if (line.compare(first, 2, "::") == 0)
				return true;
			std::string word = line.substr(first, 4);
			std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(g_ascii_tolower(c)); });
			return word == "rem" || word == "rem " || word == "rem\t";
		});
	}
	std::string result = text;
	const std::size_t size = text.size();
	std::size_t i = 0;
	auto at = [&](std::size_t index, const char *literal) { return text.compare(index, std::char_traits<char>::length(literal), literal) == 0; };
	while (i < size)
	{
		const char c = text[i];
		const char next = i + 1 < size ? text[i + 1] : '\0';
		std::size_t end = i;
		if (rules.preprocessor && c == '#' && onlySpacesBefore(text, i))
		{
			end = i;
			do
			{
				end = lineEnd(text, end);
				const bool continued = end > 0 && end < size && (text[end - 1] == '\\' || (text[end - 1] == '\r' && end > 1 && text[end - 2] == '\\'));
				if (!continued)
					break;
				++end;
			} while (end < size);
		}
		else if (rules.slashComments && c == '/' && next == '/')
			end = lineEnd(text, i);
		else if (rules.blockComments && c == '/' && next == '*')
		{
			const auto close = text.find("*/", i + 2);
			end = close == std::string::npos ? size : close + 2;
		}
		else if (rules.haskellBlocks && c == '{' && next == '-')
		{
			const auto close = text.find("-}", i + 2);
			end = close == std::string::npos ? size : close + 2;
		}
		else if (rules.luaBlocks && (at(i, "--[[") || at(i, "--[=[")))
		{
			const auto close = text.find(at(i, "--[[") ? "]]" : "]=]", i + 4);
			end = close == std::string::npos ? size : close + (at(i, "--[[") ? 2 : 3);
		}
		else if (rules.dashComments && c == '-' && next == '-')
			end = lineEnd(text, i);
		else if (rules.hashComments && c == '#' && (!rules.hashAfterSpaceOnly || i == 0 || g_ascii_isspace(text[i - 1])))
			end = lineEnd(text, i);
		else if (rules.semicolonComments && c == ';' && (i == 0 || g_ascii_isspace(text[i - 1])))
			end = lineEnd(text, i);
		else if (rules.apostropheComments && c == '\'')
			end = lineEnd(text, i);
		else if (rules.percentComments && c == '%' && (i == 0 || text[i - 1] != '\\'))
			end = lineEnd(text, i);
		else if (rules.tripleQuotes && (c == '"' || c == '\'') && next == c && i + 2 < size && text[i + 2] == c)
		{
			const auto close = text.find(std::string(3, c), i + 3);
			end = close == std::string::npos ? size : close + 3;
		}
		else if (rules.backtickStrings && c == '`')
			end = quotedEnd(text, i, '`', true);
		else if (rules.doubleQuoteStrings && c == '"')
			end = quotedEnd(text, i, '"', false);
		else if (rules.singleQuoteStrings && c == '\'')
			end = quotedEnd(text, i, '\'', false);
		else if (rules.charLiterals && c == '\'')
		{
			// Character literals ('a', '\n', 'é'); Rust lifetimes ('a) are left alone.
			if (next == '\\')
			{
				const auto close = text.find('\'', i + 2);
				if (close != std::string::npos && close - i <= 12)
					end = close + 1;
			}
			else if (i + 1 < size)
			{
				const char *character = text.c_str() + i + 1;
				const std::size_t length = static_cast<std::size_t>(g_utf8_next_char(character) - character);
				if (i + 1 + length < size && text[i + 1 + length] == '\'' && next != '\'')
					end = i + 2 + length;
			}
		}
		if (end > i)
		{
			blank(result, i, end);
			i = end;
		}
		else
			++i;
	}
	return result;
}

namespace
{
// ---------------------------------------------------------------- brace languages

enum class HeaderKind
{
	None,
	Container,
	Function
};

struct HeaderResult
{
	HeaderKind kind = HeaderKind::None;
	std::string name;
	std::string label;
	std::size_t offset = 0;
};

const std::set<std::string> &controlKeywords()
{
	static const std::set<std::string> keywords = {
		"if", "for", "while", "switch", "catch", "return", "sizeof", "else", "do", "try", "new", "throw",
		"case", "using", "lock", "foreach", "synchronized", "with", "elif", "when", "match", "unless",
		"defer", "go", "select", "typeof", "delete", "await", "yield", "static_assert", "decltype",
		"alignof", "assert", "__attribute__", "elseif", "fixed", "checked", "unchecked", "unsafe",
		"function", "fn", "func", "constructor"};
	return keywords;
}

void blankPattern(std::string &text, const char *pattern, int flags = 0)
{
	std::vector<std::pair<int, int>> ranges;
	searchAll(pattern, flags, text, [&](const RegexMatch &match) {
		if (match.has(1))
			ranges.push_back(match.groups[1]);
		else
			ranges.push_back(match.groups[0]);
	});
	for (const auto &[start, end] : ranges)
		blank(text, static_cast<std::size_t>(start), static_cast<std::size_t>(end));
}

void blankTemplates(std::string &text)
{
	std::size_t position = 0;
	while ((position = text.find("template", position)) != std::string::npos)
	{
		const bool wordStart = position == 0 || !(g_ascii_isalnum(text[position - 1]) || text[position - 1] == '_');
		std::size_t i = position + 8;
		while (i < text.size() && g_ascii_isspace(text[i]))
			++i;
		if (!wordStart || i >= text.size() || text[i] != '<')
		{
			position += 8;
			continue;
		}
		int depth = 0;
		for (; i < text.size(); ++i)
		{
			if (text[i] == '<')
				++depth;
			else if (text[i] == '>' && --depth == 0)
				break;
		}
		blank(text, position, std::min(i + 1, text.size()));
		position = i;
	}
}

bool hasAssignment(const std::string &text)
{
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		if (text[i] != '=')
			continue;
		const char before = i > 0 ? text[i - 1] : '\0';
		const char after = i + 1 < text.size() ? text[i + 1] : '\0';
		if (after == '=' || after == '>' || before == '=' || before == '!' || before == '<' || before == '>')
			continue;
		return true;
	}
	return false;
}

std::string containerLabel(const std::string &keyword)
{
	if (keyword == "struct" || keyword == "union")
		return "struct";
	if (keyword == "interface" || keyword == "trait")
		return "interface";
	if (keyword == "enum")
		return "enum";
	if (keyword == "namespace")
		return "namespace";
	if (keyword == "mod")
		return "módulo";
	return "classe";
}

HeaderResult analyseHeader(const std::string &id, std::string header)
{
	HeaderResult result;
	if (id == "java" || id == "javascript" || id == "dart")
		blankPattern(header, "@[A-Za-z_][\\w.]*(?:\\s*\\([^()]*\\))?");
	if (id == "java")
		blankPattern(header, "(?:^|\\n)[ \\t]*(\\[[^\\[\\]\\n]*\\])", G_REGEX_MULTILINE);
	if (id == "cpp")
	{
		blankPattern(header, "\\[\\[[^\\]]*\\]\\]");
		blankTemplates(header);
	}
	const std::size_t firstParen = header.find('(');
	const std::string prefix = firstParen == std::string::npos ? header : header.substr(0, firstParen);
	RegexMatch match;

	if (id == "go" && searchFirst("\\btype\\s+([A-Za-z_]\\w*)\\s+(struct|interface)\\b", 0, prefix, match))
	{
		result = {HeaderKind::Container, match.text(prefix, 1), match.text(prefix, 2) == "struct" ? "struct" : "interface",
			static_cast<std::size_t>(match.groups[1].first)};
		return result;
	}
	if (id == "rust" && searchFirst("(?:^|[^\\w])impl\\b(?:\\s*<[^{]*?>)?\\s+([^{]+?)\\s*$", 0, header, match))
	{
		result = {HeaderKind::Container, collapseSpaces(match.text(header, 1)), "impl", static_cast<std::size_t>(match.groups[1].first)};
		return result;
	}
	if (!hasAssignment(prefix)
		&& searchFirst("(?:^|[^\\w$.])(class|struct|interface|enum|namespace|trait|mod|record|object|union|mixin|extension)\\s+"
			"(?:class\\s+|struct\\s+)?(?:[A-Z][A-Z0-9_]+\\s+)?([A-Za-z_$][\\w$:.]*)"
			"(\\s*(?:<[^()]*>)?\\s*(?:(?::|\\bextends\\b|\\bimplements\\b|\\bwith\\b|\\bwhere\\b|\\bfor\\b|\\bon\\b|\\bfinal\\b|\\bsealed\\b)[^()]*)?)$",
			0, prefix, match))
	{
		result = {HeaderKind::Container, match.text(prefix, 2), containerLabel(match.text(prefix, 1)), static_cast<std::size_t>(match.groups[2].first)};
		return result;
	}
	if ((id == "go" || id == "rust" || id == "php" || id == "javascript")
		&& searchFirst("(?:^|[^\\w$.])(?:func|fn|function)\\b\\s*\\*?\\s*(?:\\([^()]*\\)\\s*)?([A-Za-z_$][\\w$]*)\\s*(?:<[^()]*>)?\\s*\\(", 0, header, match))
	{
		result = {HeaderKind::Function, match.text(header, 1), "função", static_cast<std::size_t>(match.groups[1].first)};
		return result;
	}
	if (id == "javascript"
		&& searchFirst("(?:^|[^\\w$.])([A-Za-z_$][\\w$]*)\\s*[:=]\\s*(?:async\\s+)?(?:function\\b\\s*\\*?\\s*[\\w$]*\\s*\\(|(?:\\([^()]*\\)|[A-Za-z_$][\\w$]*)\\s*=>\\s*$)", 0, header, match))
	{
		result = {HeaderKind::Function, match.text(header, 1), "função", static_cast<std::size_t>(match.groups[1].first)};
		return result;
	}
	if (id != "cpp" && id != "java" && id != "javascript" && id != "dart")
		return result;
	if (firstParen == std::string::npos)
		return result;
	if (id == "javascript" && (header.find("=>") != std::string::npos || header.find("function") != std::string::npos))
		return result;
	if (!searchFirst("((?:[A-Za-z_$~][\\w$]*\\s*::\\s*)*(?:operator\\s*[^\\w\\s(]{1,3}|~?[A-Za-z_$][\\w$]*))\\s*(?:<[^()]*>)?\\s*$", 0, prefix, match))
		return result;
	const std::string qualified = match.text(prefix, 1);
	const std::size_t nameStart = static_cast<std::size_t>(match.groups[1].first);
	std::string simple = qualified.substr(qualified.rfind(':') == std::string::npos ? 0 : qualified.rfind(':') + 1);
	if (!simple.empty() && simple.front() == '~')
		simple.erase(0, 1);
	if (controlKeywords().count(simple))
		return result;
	const std::string before = prefix.substr(0, nameStart);
	const auto lastChar = before.find_last_not_of(" \t\r\n");
	if (lastChar != std::string::npos && (before[lastChar] == '.' || (before[lastChar] == '>' && lastChar > 0 && before[lastChar - 1] == '-')))
		return result;
	if (hasAssignment(before) || before.find(')') != std::string::npos)
		return result;
	RegexMatch previous;
	if (searchFirst("([A-Za-z_]\\w*)\\s*$", 0, before, previous))
	{
		static const std::set<std::string> rejected = {"new", "return", "throw", "else", "case", "await", "yield", "typeof", "delete", "in", "of", "goto"};
		if (rejected.count(previous.text(before, 1)))
			return result;
	}
	int depth = 0;
	std::size_t close = std::string::npos;
	for (std::size_t i = firstParen; i < header.size(); ++i)
	{
		if (header[i] == '(')
			++depth;
		else if (header[i] == ')' && --depth == 0)
		{
			close = i;
			break;
		}
	}
	if (close == std::string::npos)
		return result;
	const std::string rest = header.substr(close + 1);
	if (hasAssignment(rest) || rest.find_first_of("?\";|+!") != std::string::npos || rest.find("&&") != std::string::npos)
		return result;
	std::string name;
	for (const char c : qualified)
	{
		if (!g_ascii_isspace(c))
			name += c;
	}
	result = {HeaderKind::Function, name, "função", nameStart};
	return result;
}

struct LineIndex
{
	explicit LineIndex(const std::string &text)
	{
		starts.push_back(0);
		for (std::size_t i = 0; i < text.size(); ++i)
		{
			if (text[i] == '\n')
				starts.push_back(i + 1);
		}
	}

	std::size_t lineOf(std::size_t offset) const
	{
		return static_cast<std::size_t>(std::upper_bound(starts.begin(), starts.end(), offset) - starts.begin());
	}

	std::vector<std::size_t> starts;
};

std::vector<FunctionEntry> parseBraces(const std::string &id, const std::string &clean)
{
	struct Scope
	{
		HeaderKind kind;
		std::string name;
		std::string label;
	};
	std::vector<FunctionEntry> entries;
	std::vector<Scope> stack;
	std::size_t boundary = 0;
	for (std::size_t i = 0; i < clean.size(); ++i)
	{
		const char c = clean[i];
		if (c == ';')
			boundary = i + 1;
		else if (c == '}')
		{
			if (!stack.empty())
				stack.pop_back();
			boundary = i + 1;
		}
		else if (c == '{')
		{
			const HeaderResult header = analyseHeader(id, clean.substr(boundary, i - boundary));
			const bool insideFunction = std::any_of(stack.begin(), stack.end(), [](const Scope &scope) { return scope.kind == HeaderKind::Function; });
			if (header.kind != HeaderKind::None && !header.name.empty() && !insideFunction)
			{
				FunctionEntry entry;
				entry.name = header.name;
				entry.position = boundary + header.offset;
				entry.container = header.kind == HeaderKind::Container;
				std::string parentLabel;
				for (auto scope = stack.rbegin(); scope != stack.rend(); ++scope)
				{
					if (scope->kind == HeaderKind::Container)
					{
						entry.parent = scope->name;
						parentLabel = scope->label;
						break;
					}
				}
				const bool inType = !entry.parent.empty() && parentLabel != "namespace" && parentLabel != "módulo";
				entry.kind = entry.container ? header.label : (inType ? "método" : "função");
				entries.push_back(std::move(entry));
			}
			stack.push_back({header.kind, header.name, header.label});
			boundary = i + 1;
		}
	}
	return entries;
}

std::vector<FunctionEntry> parsePython(const std::string &clean)
{
	struct Scope
	{
		std::size_t indent;
		bool isClass;
		std::string name;
	};
	std::vector<FunctionEntry> entries;
	std::vector<Scope> stack;
	searchAll("^([ \\t]*)(?:async[ \\t]+)?(def|class)[ \\t]+([A-Za-z_]\\w*)", G_REGEX_MULTILINE, clean, [&](const RegexMatch &match) {
		std::size_t indent = 0;
		for (const char c : match.text(clean, 1))
			indent += c == '\t' ? 8 : 1;
		while (!stack.empty() && stack.back().indent >= indent)
			stack.pop_back();
		const bool isClass = match.text(clean, 2) == "class";
		const bool nested = std::any_of(stack.begin(), stack.end(), [](const Scope &scope) { return !scope.isClass; });
		const std::string name = match.text(clean, 3);
		if (!nested)
		{
			FunctionEntry entry;
			entry.name = name;
			entry.position = static_cast<std::size_t>(match.groups[3].first);
			entry.container = isClass;
			entry.parent = !stack.empty() ? stack.back().name : std::string();
			entry.kind = isClass ? "classe" : (entry.parent.empty() ? "função" : "método");
			entries.push_back(std::move(entry));
		}
		stack.push_back({indent, isClass, name});
	});
	return entries;
}

struct LinePattern
{
	const char *pattern;
	int flags;
	std::size_t nameGroup;
	const char *kind;
	std::size_t alternativeGroup = 0; // used as the name when it matched
	std::size_t kindGroup = 0;        // lower-cased into the kind when it matched
	bool container = false;
};

const std::map<std::string, std::vector<LinePattern>> &linePatterns()
{
	constexpr int multi = G_REGEX_MULTILINE;
	constexpr int caseless = G_REGEX_MULTILINE | G_REGEX_CASELESS;
	static const std::map<std::string, std::vector<LinePattern>> patterns = {
		{"shell", {
			{"^[ \\t]*(?:function[ \\t]+)?([A-Za-z_][\\w.:-]*)[ \\t]*\\([ \\t]*\\)", multi, 1, "função"},
			{"^[ \\t]*function[ \\t]+([A-Za-z_][\\w.:-]*)[ \\t]*(?:\\{|$)", multi, 1, "função"}}},
		{"lua", {
			{"^[ \\t]*(?:local[ \\t]+)?function[ \\t]+([\\w.:]+)", multi, 1, "função"},
			{"^[ \\t]*(?:local[ \\t]+)?([\\w.]+)[ \\t]*=[ \\t]*function\\b", multi, 1, "função"}}},
		{"perl", {
			{"^[ \\t]*package[ \\t]+([\\w:]+)", multi, 1, "pacote", 0, 0, true},
			{"^[ \\t]*sub[ \\t]+([\\w:]+)", multi, 1, "função"}}},
		{"ruby", {
			{"^[ \\t]*(class|module)[ \\t]+([A-Z][\\w:]*)", multi, 2, "classe", 0, 0, true},
			{"^[ \\t]*def[ \\t]+((?:self\\.)?[\\w?!=\\[\\]<>+*/%-]+)", multi, 1, "método"}}},
		{"make", {
			{"^([A-Za-z0-9_/%$(){}][A-Za-z0-9_./%$(){} -]*?)[ \\t]*::?(?!=)", multi, 1, "alvo"}}},
		{"cmake", {
			{"^[ \\t]*(function|macro)[ \\t]*\\([ \\t]*([\\w.-]+)", caseless, 2, "função"}}},
		{"dockerfile", {
			{"^[ \\t]*FROM[ \\t]+(?:--\\S+[ \\t]+)*(\\S+)(?:[ \\t]+AS[ \\t]+(\\S+))?", caseless, 1, "estágio", 2}}},
		{"diff", {
			{"^\\+\\+\\+ (?:b/)?(\\S+)", multi, 1, "arquivo"}}},
		{"tex", {
			{"\\\\(part|chapter|section|subsection|subsubsection)\\*?\\{([^}\\n]*)\\}", 0, 2, "seção", 0, 1}}},
		{"powershell", {
			{"^[ \\t]*class[ \\t]+(\\w+)", caseless, 1, "classe", 0, 0, true},
			{"^[ \\t]*(?:function|filter)[ \\t]+([\\w-]+)", caseless, 1, "função"}}},
		{"batch", {
			{"^[ \\t]*:([A-Za-z_][\\w.-]*)", multi, 1, "rótulo"}}},
		{"nsis", {
			{"^[ \\t]*Function[ \\t]+(\\S+)", caseless, 1, "função"},
			{"^[ \\t]*Section[ \\t]+(?:/o[ \\t]+)?\"?([^\"\\r\\n]+?)\"?[ \\t]*$", caseless, 1, "seção"}}},
		{"vb", {
			{"^[ \\t]*(?:(?:Public|Private|Friend|Protected)[ \\t]+)?(Class|Module|Structure)[ \\t]+(\\w+)", caseless, 2, "classe", 0, 0, true},
			{"^[ \\t]*(?:(?:Public|Private|Friend|Protected|Shared|Static|Overrides|Overridable)[ \\t]+)*(?:Sub|Function|Property)[ \\t]+(?:(?:Get|Let|Set)[ \\t]+)?(\\w+)", caseless, 1, "função"}}},
		{"haskell", {
			{"^(?:data|newtype|type|class)[ \\t]+([A-Z][\\w']*)", multi, 1, "tipo", 0, 0, true},
			{"^([a-z_][\\w']*)[ \\t]*::", multi, 1, "função"}}},
		{"sql", {
			{"\\bcreate[ \\t]+(?:or[ \\t]+replace[ \\t]+)?(?:temp(?:orary)?[ \\t]+)?(function|procedure|table|view|trigger|index)[ \\t]+(?:if[ \\t]+not[ \\t]+exists[ \\t]+)?([\\w.\"`\\[\\]]+)", caseless, 2, "objeto", 0, 1}}},
		{"markdown", {
			{"^(#{1,6})[ \\t]+(.+?)[ \\t]*#*[ \\t]*$", multi, 2, "título"}}},
		{"toml", {
			{"^[ \\t]*\\[{1,2}([^\\]\\r\\n]+)\\]{1,2}", multi, 1, "seção", 0, 0, true}}},
		{"yaml", {
			{"^([A-Za-z_][\\w.-]*)[ \\t]*:", multi, 1, "chave"}}},
		{"css", {
			{"^[ \\t]*([^\\s{}@/;][^{};\\n]*?)[ \\t]*\\{", multi, 1, "seletor"}}},
	};
	return patterns;
}

std::vector<FunctionEntry> parseLines(const std::vector<LinePattern> &patterns, const std::string &clean, const std::string &original)
{
	std::vector<FunctionEntry> entries;
	for (const auto &pattern : patterns)
	{
		searchAll(pattern.pattern, pattern.flags, clean, [&](const RegexMatch &match) {
			const std::size_t group = pattern.alternativeGroup && match.has(pattern.alternativeGroup) ? pattern.alternativeGroup : pattern.nameGroup;
			if (!match.has(group))
				return;
			FunctionEntry entry;
			entry.position = static_cast<std::size_t>(match.groups[group].first);
			entry.name = collapseSpaces(original.substr(entry.position, static_cast<std::size_t>(match.groups[group].second) - entry.position));
			entry.kind = pattern.kind;
			entry.container = pattern.container;
			if (pattern.kindGroup && match.has(pattern.kindGroup))
			{
				entry.kind = match.text(clean, pattern.kindGroup);
				std::transform(entry.kind.begin(), entry.kind.end(), entry.kind.begin(), [](unsigned char c) { return static_cast<char>(g_ascii_tolower(c)); });
			}
			if (std::string(pattern.kind) == "título" && match.has(1))
				entry.kind = "H" + std::to_string(match.groups[1].second - match.groups[1].first);
			if (entry.name.empty() || (entry.kind == "alvo" && entry.name.front() == '.'))
				return;
			entries.push_back(std::move(entry));
		});
	}
	std::sort(entries.begin(), entries.end(), [](const FunctionEntry &a, const FunctionEntry &b) { return a.position < b.position; });
	entries.erase(std::unique(entries.begin(), entries.end(), [](const FunctionEntry &a, const FunctionEntry &b) { return a.position == b.position; }), entries.end());
	return entries;
}

bool isBraceLanguage(const std::string &id)
{
	return id == "cpp" || id == "java" || id == "javascript" || id == "rust" || id == "go" || id == "php" || id == "dart";
}
}

bool functionListSupports(const std::string &languageId)
{
	return isBraceLanguage(languageId) || languageId == "python" || linePatterns().count(languageId) > 0;
}

std::vector<FunctionEntry> parseFunctions(const std::string &languageId, const std::string &text)
{
	if (!functionListSupports(languageId))
		return {};
	const std::string clean = blankCommentsAndStrings(languageId, text);
	std::vector<FunctionEntry> entries;
	if (isBraceLanguage(languageId))
		entries = parseBraces(languageId, clean);
	else if (languageId == "python")
		entries = parsePython(clean);
	else
		entries = parseLines(linePatterns().at(languageId), clean, text);
	const LineIndex index(text);
	for (auto &entry : entries)
		entry.line = index.lineOf(entry.position);
	return entries;
}
}
