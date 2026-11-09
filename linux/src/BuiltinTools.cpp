#include "BuiltinTools.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include <glib.h>

namespace NppLinux
{
std::string base64Encode(const std::string &data)
{
	gchar *encoded = g_base64_encode(reinterpret_cast<const guchar *>(data.data()), data.size());
	std::string result = encoded ? encoded : "";
	g_free(encoded);
	return result;
}

bool base64Decode(const std::string &text, std::string &data)
{
	std::string clean;
	for (const char c : text)
	{
		if (g_ascii_isalnum(c) || c == '+' || c == '/' || c == '=')
			clean += c;
		else if (c == '-')
			clean += '+'; // URL-safe alphabet
		else if (c == '_')
			clean += '/';
		else if (!g_ascii_isspace(c))
			return false;
	}
	if (clean.empty())
		return false;
	while (clean.size() % 4)
		clean += '=';
	gsize length = 0;
	guchar *decoded = g_base64_decode(clean.c_str(), &length);
	if (!decoded)
		return false;
	data.assign(reinterpret_cast<const char *>(decoded), length);
	g_free(decoded);
	return true;
}

std::string urlEncode(const std::string &text)
{
	gchar *escaped = g_uri_escape_string(text.c_str(), nullptr, FALSE);
	std::string result = escaped ? escaped : "";
	g_free(escaped);
	return result;
}

bool urlDecode(const std::string &text, std::string &decoded)
{
	std::string plus = text;
	std::replace(plus.begin(), plus.end(), '+', ' ');
	gchar *unescaped = g_uri_unescape_string(plus.c_str(), nullptr);
	if (!unescaped)
		return false;
	decoded = unescaped;
	g_free(unescaped);
	return true;
}

std::string asciiToHex(const std::string &text)
{
	static const char digits[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(text.size() * 2);
	for (const unsigned char c : text)
	{
		result += digits[c >> 4];
		result += digits[c & 15];
	}
	return result;
}

bool hexToAscii(const std::string &hex, std::string &text)
{
	std::string digits;
	for (std::size_t i = 0; i < hex.size(); ++i)
	{
		const char c = hex[i];
		if (c == '0' && i + 1 < hex.size() && (hex[i + 1] == 'x' || hex[i + 1] == 'X'))
		{
			++i;
			continue;
		}
		if (g_ascii_isxdigit(c))
			digits += c;
		else if (!g_ascii_isspace(c) && c != ',' && c != ':' && c != '-')
			return false;
	}
	if (digits.empty() || digits.size() % 2)
		return false;
	text.clear();
	for (std::size_t i = 0; i < digits.size(); i += 2)
		text += static_cast<char>(g_ascii_xdigit_value(digits[i]) * 16 + g_ascii_xdigit_value(digits[i + 1]));
	return true;
}

namespace
{
// Port of jsmin.c (Douglas Crockford, 2002-2019) working on strings.
class JsMin
{
public:
	explicit JsMin(const std::string &source) : _in(source) {}

	bool run(std::string &result, std::string &error)
	{
		if (_in.compare(0, 3, "\xEF\xBB\xBF") == 0)
			_pos = 3;
		_a = '\n';
		action(3);
		while (_a != End && _error.empty())
		{
			switch (_a)
			{
				case ' ':
					action(alphanum(_b) ? 1 : 2);
					break;
				case '\n':
					switch (_b)
					{
						case '{': case '[': case '(': case '+': case '-': case '!': case '~':
							action(1);
							break;
						case ' ':
							action(3);
							break;
						default:
							action(alphanum(_b) ? 1 : 2);
					}
					break;
				default:
					switch (_b)
					{
						case ' ':
							action(alphanum(_a) ? 1 : 3);
							break;
						case '\n':
							switch (_a)
							{
								case '}': case ']': case ')': case '+': case '-': case '"': case '\'': case '`':
									action(1);
									break;
								default:
									action(alphanum(_a) ? 1 : 3);
							}
							break;
						default:
							action(1);
					}
			}
		}
		if (!_error.empty())
		{
			error = _error;
			return false;
		}
		while (!_out.empty() && _out.front() == '\n')
			_out.erase(_out.begin());
		result = _out;
		return true;
	}

private:
	static constexpr int End = -1;

	static bool alphanum(int c)
	{
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
			|| c == '_' || c == '$' || c == '\\' || c > 126;
	}

	int get()
	{
		int c = _lookahead;
		_lookahead = End;
		if (c == End)
			c = _pos < _in.size() ? static_cast<unsigned char>(_in[_pos++]) : End;
		if (c >= ' ' || c == '\n' || c == End)
			return c;
		if (c == '\r')
			return '\n';
		return ' ';
	}

	int peek()
	{
		_lookahead = get();
		return _lookahead;
	}

	int next()
	{
		int c = get();
		if (c == '/')
		{
			switch (peek())
			{
				case '/':
					for (;;)
					{
						c = get();
						if (c <= '\n')
							break;
					}
					break;
				case '*':
					get();
					while (c != ' ')
					{
						switch (get())
						{
							case '*':
								if (peek() == '/')
								{
									get();
									c = ' ';
								}
								break;
							case End:
								_error = "Comentário /* sem fim.";
								return End;
						}
					}
					break;
			}
		}
		_y = _x;
		_x = c;
		return c;
	}

	void put(int c)
	{
		if (c != End)
			_out.push_back(static_cast<char>(c));
	}

	void action(int step)
	{
		if (step <= 1)
		{
			put(_a);
			if ((_y == '\n' || _y == ' ') && (_a == '+' || _a == '-' || _a == '*' || _a == '/')
				&& (_b == '+' || _b == '-' || _b == '*' || _b == '/'))
				put(_y);
		}
		if (step <= 2)
		{
			_a = _b;
			if (_a == '\'' || _a == '"' || _a == '`')
			{
				for (;;)
				{
					put(_a);
					_a = get();
					if (_a == _b)
						break;
					if (_a == '\\')
					{
						put(_a);
						_a = get();
					}
					if (_a == End)
					{
						_error = "Texto entre aspas sem fim.";
						return;
					}
				}
			}
		}
		_b = next();
		if (_b == '/' && (_a == '(' || _a == ',' || _a == '=' || _a == ':' || _a == '[' || _a == '!' || _a == '&' || _a == '|'
			|| _a == '?' || _a == '+' || _a == '-' || _a == '~' || _a == '*' || _a == '/' || _a == '{' || _a == '}' || _a == ';'))
		{
			put(_a);
			if (_a == '/' || _a == '*')
				put(' ');
			put(_b);
			for (;;)
			{
				_a = get();
				if (_a == '[')
				{
					for (;;)
					{
						put(_a);
						_a = get();
						if (_a == ']')
							break;
						if (_a == '\\')
						{
							put(_a);
							_a = get();
						}
						if (_a == End)
						{
							_error = "Expressão regular sem fim.";
							return;
						}
					}
				}
				else if (_a == '/')
				{
					const int following = peek();
					if (following == '/' || following == '*')
					{
						_error = "Expressão regular sem fim.";
						return;
					}
					break;
				}
				else if (_a == '\\')
				{
					put(_a);
					_a = get();
				}
				if (_a == End)
				{
					_error = "Expressão regular sem fim.";
					return;
				}
				put(_a);
			}
			_b = next();
		}
	}

	const std::string &_in;
	std::size_t _pos = 0;
	std::string _out;
	std::string _error;
	int _a = End;
	int _b = End;
	int _lookahead = End;
	int _x = End;
	int _y = End;
};
}

bool jsMinify(const std::string &source, std::string &result, std::string &error)
{
	JsMin minifier(source);
	return minifier.run(result, error);
}

std::string jsFormat(const std::string &source, const std::string &indent)
{
	std::string out;
	int depth = 0;
	int parens = 0;
	bool lineStart = true;
	auto newline = [&]() {
		while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
			out.pop_back();
		if (!out.empty() && out.back() != '\n')
			out += '\n';
		lineStart = true;
	};
	auto write = [&](const std::string &text) {
		if (lineStart)
		{
			for (int i = 0; i < depth; ++i)
				out += indent;
			lineStart = false;
		}
		out += text;
	};
	auto nextSignificant = [&](std::size_t from) {
		while (from < source.size() && (source[from] == ' ' || source[from] == '\t' || source[from] == '\r'))
			++from;
		return from < source.size() ? source[from] : '\0';
	};
	for (std::size_t i = 0; i < source.size(); ++i)
	{
		const char c = source[i];
		if (c == '\r')
			continue;
		if (c == '\n')
		{
			newline();
			continue;
		}
		if ((c == ' ' || c == '\t') && lineStart)
			continue;
		if (c == '"' || c == '\'' || c == '`')
		{
			std::size_t end = i + 1;
			while (end < source.size() && source[end] != c && (c == '`' || source[end] != '\n'))
				end += source[end] == '\\' ? 2 : 1;
			write(source.substr(i, std::min(end, source.size() - 1) - i + 1));
			i = end;
			continue;
		}
		if (c == '/' && i + 1 < source.size() && source[i + 1] == '/')
		{
			const auto end = source.find('\n', i);
			std::string comment = source.substr(i, end == std::string::npos ? std::string::npos : end - i);
			if (!comment.empty() && comment.back() == '\r')
				comment.pop_back();
			write(comment);
			newline();
			i = end == std::string::npos ? source.size() : end;
			continue;
		}
		if (c == '/' && i + 1 < source.size() && source[i + 1] == '*')
		{
			const auto end = source.find("*/", i + 2);
			const std::size_t stop = end == std::string::npos ? source.size() : end + 2;
			write(source.substr(i, stop - i));
			i = stop - 1;
			continue;
		}
		switch (c)
		{
			case '{':
				write("{");
				++depth;
				newline();
				break;
			case '}':
				newline();
				depth = std::max(0, depth - 1);
				write("}");
				if (nextSignificant(i + 1) != ';' && nextSignificant(i + 1) != ',' && nextSignificant(i + 1) != ')')
					newline();
				break;
			case '(':
				++parens;
				write("(");
				break;
			case ')':
				parens = std::max(0, parens - 1);
				write(")");
				break;
			case ';':
				write(";");
				if (parens == 0)
					newline();
				break;
			default:
				write(std::string(1, c));
		}
	}
	newline();
	// Several blank lines become one.
	std::string compact;
	for (const char c : out)
	{
		if (c == '\n' && compact.size() >= 2 && compact[compact.size() - 1] == '\n' && compact[compact.size() - 2] == '\n')
			continue;
		compact += c;
	}
	return compact;
}

bool expandExtended(const std::string &text, std::string &result)
{
	result.clear();
	auto appendCode = [&](unsigned long code) {
		char buffer[8];
		const int length = g_unichar_to_utf8(static_cast<gunichar>(code), buffer);
		result.append(buffer, static_cast<std::size_t>(length));
	};
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		if (text[i] != '\\' || i + 1 >= text.size())
		{
			result += text[i];
			continue;
		}
		const char kind = text[++i];
		auto number = [&](std::size_t digits, int base) -> bool {
			const std::string part = text.substr(i + 1, digits);
			if (part.size() != digits)
				return false;
			char *end = nullptr;
			const unsigned long code = std::strtoul(part.c_str(), &end, base);
			if (!end || *end)
				return false;
			appendCode(code);
			i += digits;
			return true;
		};
		switch (kind)
		{
			case 'n': result += '\n'; break;
			case 'r': result += '\r'; break;
			case 't': result += '\t'; break;
			case '0': result += '\0'; break;
			case '\\': result += '\\'; break;
			case 'x': if (!number(2, 16)) return false; break;
			case 'u': if (!number(4, 16)) return false; break;
			case 'o': if (!number(3, 8)) return false; break;
			case 'd': if (!number(3, 10)) return false; break;
			case 'b': if (!number(8, 2)) return false; break;
			default:
				result += '\\';
				result += kind;
		}
	}
	return true;
}

std::vector<std::string> splitLines(const std::string &text)
{
	std::vector<std::string> lines;
	std::size_t start = 0;
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		if (text[i] == '\n' || text[i] == '\r')
		{
			lines.push_back(text.substr(start, i - start));
			if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
				++i;
			start = i + 1;
		}
	}
	if (start < text.size())
		lines.push_back(text.substr(start));
	return lines;
}

std::vector<DiffLine> diffLines(const std::vector<std::string> &left, const std::vector<std::string> &right)
{
	std::vector<DiffLine> result;
	std::size_t prefix = 0;
	while (prefix < left.size() && prefix < right.size() && left[prefix] == right[prefix])
		++prefix;
	std::size_t suffix = 0;
	while (suffix < left.size() - prefix && suffix < right.size() - prefix
		&& left[left.size() - 1 - suffix] == right[right.size() - 1 - suffix])
		++suffix;
	for (std::size_t i = 0; i < prefix; ++i)
		result.push_back({' ', i, i, left[i]});
	const std::size_t n = left.size() - prefix - suffix;
	const std::size_t m = right.size() - prefix - suffix;
	if (n * m <= 25000000)
	{
		// Longest common subsequence of the middle part.
		std::vector<std::vector<std::uint32_t>> table(n + 1, std::vector<std::uint32_t>(m + 1, 0));
		for (std::size_t i = n; i-- > 0;)
		{
			for (std::size_t j = m; j-- > 0;)
				table[i][j] = left[prefix + i] == right[prefix + j] ? table[i + 1][j + 1] + 1 : std::max(table[i + 1][j], table[i][j + 1]);
		}
		std::size_t i = 0;
		std::size_t j = 0;
		while (i < n || j < m)
		{
			if (i < n && j < m && left[prefix + i] == right[prefix + j])
			{
				result.push_back({' ', prefix + i, prefix + j, left[prefix + i]});
				++i;
				++j;
			}
			else if (i < n && (j == m || table[i + 1][j] >= table[i][j + 1]))
			{
				// Removed lines come before added ones, as in other diff tools.
				result.push_back({'-', prefix + i, prefix + j, left[prefix + i]});
				++i;
			}
			else
			{
				result.push_back({'+', prefix + i, prefix + j, right[prefix + j]});
				++j;
			}
		}
	}
	else
	{
		// Very large differences: everything in the middle counts as changed.
		for (std::size_t i = 0; i < n; ++i)
			result.push_back({'-', prefix + i, prefix, left[prefix + i]});
		for (std::size_t j = 0; j < m; ++j)
			result.push_back({'+', prefix + n, prefix + j, right[prefix + j]});
	}
	for (std::size_t k = 0; k < suffix; ++k)
		result.push_back({' ', left.size() - suffix + k, right.size() - suffix + k, left[left.size() - suffix + k]});
	return result;
}

std::string unifiedDiff(const std::vector<DiffLine> &diff, const std::string &leftName, const std::string &rightName)
{
	const std::size_t context = 3;
	std::vector<bool> keep(diff.size(), false);
	bool changed = false;
	for (std::size_t i = 0; i < diff.size(); ++i)
	{
		if (diff[i].kind == ' ')
			continue;
		changed = true;
		const std::size_t from = i >= context ? i - context : 0;
		for (std::size_t k = from; k < diff.size() && k <= i + context; ++k)
			keep[k] = true;
	}
	if (!changed)
		return {};
	std::string out = "--- " + leftName + "\n+++ " + rightName + "\n";
	std::size_t i = 0;
	while (i < diff.size())
	{
		if (!keep[i])
		{
			++i;
			continue;
		}
		std::size_t end = i;
		while (end < diff.size() && keep[end])
			++end;
		std::size_t leftCount = 0;
		std::size_t rightCount = 0;
		for (std::size_t k = i; k < end; ++k)
		{
			leftCount += diff[k].kind != '+';
			rightCount += diff[k].kind != '-';
		}
		out += "@@ -" + std::to_string(diff[i].left + 1) + "," + std::to_string(leftCount)
			+ " +" + std::to_string(diff[i].right + 1) + "," + std::to_string(rightCount) + " @@\n";
		for (std::size_t k = i; k < end; ++k)
			out += std::string(1, diff[k].kind) + diff[k].text + "\n";
		i = end;
	}
	return out;
}

std::vector<CompareRow> alignDiff(const std::vector<DiffLine> &diff)
{
	std::vector<CompareRow> rows;
	std::size_t i = 0;
	while (i < diff.size())
	{
		if (diff[i].kind == ' ')
		{
			rows.push_back({' ', static_cast<long>(diff[i].left), static_cast<long>(diff[i].right)});
			++i;
			continue;
		}
		std::vector<long> removed;
		std::vector<long> added;
		for (; i < diff.size() && diff[i].kind != ' '; ++i)
		{
			if (diff[i].kind == '-')
				removed.push_back(static_cast<long>(diff[i].left));
			else
				added.push_back(static_cast<long>(diff[i].right));
		}
		const std::size_t paired = std::min(removed.size(), added.size());
		for (std::size_t k = 0; k < paired; ++k)
			rows.push_back({'~', removed[k], added[k]});
		for (std::size_t k = paired; k < removed.size(); ++k)
			rows.push_back({'-', removed[k], -1});
		for (std::size_t k = paired; k < added.size(); ++k)
			rows.push_back({'+', -1, added[k]});
	}
	return rows;
}

std::string compareKey(const std::string &line, bool ignoreSpaces, bool ignoreCase)
{
	if (!ignoreSpaces && !ignoreCase)
		return line;
	std::string key;
	key.reserve(line.size());
	for (const char c : line)
	{
		if (ignoreSpaces && (c == ' ' || c == '\t' || c == '\v' || c == '\f'))
			continue;
		key += ignoreCase ? g_ascii_tolower(c) : c;
	}
	if (ignoreCase && !g_utf8_validate(key.c_str(), static_cast<gssize>(key.size()), nullptr))
		return key;
	if (ignoreCase)
	{
		gchar *folded = g_utf8_casefold(key.c_str(), static_cast<gssize>(key.size()));
		key = folded ? folded : key;
		g_free(folded);
	}
	return key;
}

namespace
{
// Words (letters, digits, _ and any non-ASCII character), runs of spaces, or a
// single punctuation character.
std::vector<std::pair<std::size_t, std::size_t>> tokens(const std::string &line)
{
	std::vector<std::pair<std::size_t, std::size_t>> result;
	auto word = [](unsigned char c) { return g_ascii_isalnum(c) || c == '_' || c >= 0x80; };
	auto space = [](unsigned char c) { return c == ' ' || c == '\t'; };
	std::size_t i = 0;
	while (i < line.size())
	{
		const auto c = static_cast<unsigned char>(line[i]);
		std::size_t end = i + 1;
		if (word(c))
			while (end < line.size() && word(static_cast<unsigned char>(line[end])))
				++end;
		else if (space(c))
			while (end < line.size() && space(static_cast<unsigned char>(line[end])))
				++end;
		result.push_back({i, end - i});
		i = end;
	}
	return result;
}

void addRange(std::vector<std::pair<std::size_t, std::size_t>> &ranges, std::size_t start, std::size_t length)
{
	if (!ranges.empty() && ranges.back().first + ranges.back().second == start)
		ranges.back().second += length;
	else
		ranges.push_back({start, length});
}
}

WordChanges wordDiff(const std::string &left, const std::string &right)
{
	WordChanges changes;
	const auto a = tokens(left);
	const auto b = tokens(right);
	const std::size_t n = a.size();
	const std::size_t m = b.size();
	if (n == 0 || m == 0 || n * m > 1000000)
		return changes;
	auto same = [&](std::size_t i, std::size_t j) {
		return a[i].second == b[j].second && left.compare(a[i].first, a[i].second, right, b[j].first, b[j].second) == 0;
	};
	std::vector<std::vector<std::uint32_t>> table(n + 1, std::vector<std::uint32_t>(m + 1, 0));
	for (std::size_t i = n; i-- > 0;)
		for (std::size_t j = m; j-- > 0;)
			table[i][j] = same(i, j) ? table[i + 1][j + 1] + 1 : std::max(table[i + 1][j], table[i][j + 1]);
	std::size_t i = 0;
	std::size_t j = 0;
	std::size_t commonText = 0;
	while (i < n || j < m)
	{
		if (i < n && j < m && same(i, j))
		{
			if (!g_ascii_isspace(left[a[i].first]))
				commonText += a[i].second;
			++i;
			++j;
		}
		else if (i < n && (j == m || table[i + 1][j] >= table[i][j + 1]))
		{
			addRange(changes.left, a[i].first, a[i].second);
			++i;
		}
		else
		{
			addRange(changes.right, b[j].first, b[j].second);
			++j;
		}
	}
	if (commonText == 0)
		return {};
	return changes;
}
}
