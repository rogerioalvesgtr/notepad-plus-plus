#include "TextTools.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <random>

#include <glib.h>

namespace NppLinux
{
namespace
{
std::string collateKey(const std::string &line, bool fold)
{
	if (!g_utf8_validate(line.c_str(), static_cast<gssize>(line.size()), nullptr))
		return line;
	gchar *folded = fold ? g_utf8_casefold(line.c_str(), -1) : g_strdup(line.c_str());
	gchar *key = g_utf8_collate_key(folded, -1);
	std::string result = key ? key : line;
	g_free(key);
	g_free(folded);
	return result;
}

std::string casefold(const std::string &line)
{
	if (!g_utf8_validate(line.c_str(), static_cast<gssize>(line.size()), nullptr))
		return line;
	gchar *folded = g_utf8_casefold(line.c_str(), -1);
	std::string result = folded ? folded : line;
	g_free(folded);
	return result;
}

std::optional<long double> parseNumber(const std::string &line, SortKind kind)
{
	std::string text = trimTrailing(trimLeading(line));
	if (text.empty())
		return std::nullopt;
	if (kind == SortKind::DecimalComma)
	{
		// 1.234,5 -> 1234.5
		text.erase(std::remove(text.begin(), text.end(), '.'), text.end());
		std::replace(text.begin(), text.end(), ',', '.');
	}
	else if (kind == SortKind::DecimalDot)
		text.erase(std::remove(text.begin(), text.end(), ','), text.end());
	std::size_t i = 0;
	if (text[i] == '+' || text[i] == '-')
		++i;
	bool digits = false;
	bool dot = false;
	for (; i < text.size(); ++i)
	{
		if (g_ascii_isdigit(text[i]))
			digits = true;
		else if (text[i] == '.' && kind != SortKind::Integer && !dot)
			dot = true;
		else
			return std::nullopt;
	}
	if (!digits)
		return std::nullopt;
	return std::strtold(text.c_str(), nullptr);
}

void appendUnichar(std::string &out, gunichar c)
{
	char buffer[8];
	const int length = g_unichar_to_utf8(c, buffer);
	out.append(buffer, static_cast<std::size_t>(length));
}
}

void sortLineList(std::vector<std::string> &lines, SortKind kind, bool descending)
{
	if (kind == SortKind::Integer || kind == SortKind::DecimalComma || kind == SortKind::DecimalDot)
	{
		std::vector<std::string> text;
		std::vector<std::pair<long double, std::string>> numbers;
		for (auto &line : lines)
		{
			if (const auto value = parseNumber(line, kind))
				numbers.emplace_back(*value, std::move(line));
			else
				text.push_back(std::move(line));
		}
		std::stable_sort(numbers.begin(), numbers.end(), [&](const auto &a, const auto &b) {
			return descending ? b.first < a.first : a.first < b.first;
		});
		lines.clear();
		if (!descending)
			lines = std::move(text);
		for (auto &entry : numbers)
			lines.push_back(std::move(entry.second));
		if (descending)
			lines.insert(lines.end(), std::make_move_iterator(text.begin()), std::make_move_iterator(text.end()));
		return;
	}
	std::vector<std::pair<std::string, std::string>> keyed;
	keyed.reserve(lines.size());
	for (auto &line : lines)
	{
		std::string key;
		switch (kind)
		{
			case SortKind::LexicalIgnoreCase:
				key = casefold(line);
				break;
			case SortKind::Locale:
				key = collateKey(line, false);
				break;
			case SortKind::Length:
			{
				const glong length = g_utf8_validate(line.c_str(), static_cast<gssize>(line.size()), nullptr)
					? g_utf8_strlen(line.c_str(), static_cast<gssize>(line.size())) : static_cast<glong>(line.size());
				key = std::string(20 - std::min<std::size_t>(20, std::to_string(length).size()), '0') + std::to_string(length);
				break;
			}
			default:
				key = line;
		}
		keyed.emplace_back(std::move(key), std::move(line));
	}
	std::stable_sort(keyed.begin(), keyed.end(), [&](const auto &a, const auto &b) {
		return descending ? b.first < a.first : a.first < b.first;
	});
	lines.clear();
	for (auto &entry : keyed)
		lines.push_back(std::move(entry.second));
}

std::size_t removeConsecutiveDuplicates(std::vector<std::string> &lines)
{
	const std::size_t before = lines.size();
	lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
	return before - lines.size();
}

std::size_t removeEmptyLines(std::vector<std::string> &lines, bool blankToo)
{
	const std::size_t before = lines.size();
	lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const std::string &line) {
		return blankToo ? line.find_first_not_of(" \t") == std::string::npos : line.empty();
	}), lines.end());
	return before - lines.size();
}

void shuffleLines(std::vector<std::string> &lines)
{
	std::random_device device;
	std::mt19937 generator(device());
	std::shuffle(lines.begin(), lines.end(), generator);
}

std::string convertCase(const std::string &text, CaseKind kind)
{
	if (!g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr))
		return text;
	std::string out;
	out.reserve(text.size());
	std::random_device device;
	std::mt19937 generator(device());
	bool sentenceStart = true;
	bool sentenceEndSeen = false;
	gunichar previous = 0;
	gunichar beforePrevious = 0;
	for (const char *p = text.c_str(); *p; p = g_utf8_next_char(p))
	{
		const gunichar c = g_utf8_get_char(p);
		gunichar result = c;
		const bool letter = g_unichar_isalpha(c);
		switch (kind)
		{
			case CaseKind::Upper:
				result = g_unichar_toupper(c);
				break;
			case CaseKind::Lower:
				result = g_unichar_tolower(c);
				break;
			case CaseKind::Invert:
				result = g_unichar_isupper(c) ? g_unichar_tolower(c) : g_unichar_toupper(c);
				break;
			case CaseKind::Random:
				result = (generator() & 1) ? g_unichar_toupper(c) : g_unichar_tolower(c);
				break;
			case CaseKind::Proper:
			case CaseKind::ProperBlend:
			{
				// A word starts after anything that is not a letter, digit or an apostrophe inside a word.
				const bool apostrophe = (previous == '\'' || previous == 0x2019) && g_unichar_isalnum(beforePrevious);
				const bool wordStart = !g_unichar_isalnum(previous) && !apostrophe;
				if (letter)
					result = wordStart ? g_unichar_toupper(c) : kind == CaseKind::Proper ? g_unichar_tolower(c) : c;
				break;
			}
			case CaseKind::Sentence:
			case CaseKind::SentenceBlend:
				if (letter)
				{
					result = sentenceStart ? g_unichar_toupper(c) : kind == CaseKind::Sentence ? g_unichar_tolower(c) : c;
					sentenceStart = false;
					sentenceEndSeen = false;
				}
				else if (c == '.' || c == '!' || c == '?')
					sentenceEndSeen = true;
				else if (c == '\n' || c == '\r')
				{
					if (sentenceEndSeen || previous == '\n' || previous == '\r')
						sentenceStart = true;
				}
				else if (g_unichar_isspace(c))
				{
					if (sentenceEndSeen)
						sentenceStart = true;
				}
				else if (!g_unichar_isdigit(c))
					; // Quotes and brackets keep the pending sentence start.
				else
				{
					sentenceStart = false;
					sentenceEndSeen = false;
				}
				break;
		}
		appendUnichar(out, result);
		beforePrevious = previous;
		previous = c;
	}
	return out;
}

std::string trimLeading(const std::string &line)
{
	const auto first = line.find_first_not_of(" \t");
	return first == std::string::npos ? std::string() : line.substr(first);
}

std::string trimTrailing(const std::string &line)
{
	const auto last = line.find_last_not_of(" \t");
	return last == std::string::npos ? std::string() : line.substr(0, last + 1);
}

std::string tabsToSpaces(const std::string &line, int tabWidth)
{
	const int width = std::max(1, tabWidth);
	std::string out;
	int column = 0;
	for (const char *p = line.c_str(); *p; p = g_utf8_next_char(p))
	{
		if (*p == '\t')
		{
			const int spaces = width - column % width;
			out.append(static_cast<std::size_t>(spaces), ' ');
			column += spaces;
		}
		else
		{
			out.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
			++column;
		}
	}
	return out;
}

std::string spacesToTabs(const std::string &line, int tabWidth, bool leadingOnly)
{
	const int width = std::max(1, tabWidth);
	const std::string expanded = tabsToSpaces(line, width);
	std::string out;
	int column = 0;
	int pendingSpaces = 0;
	bool leading = true;
	for (const char *p = expanded.c_str(); *p; p = g_utf8_next_char(p))
	{
		const bool convert = !leadingOnly || leading;
		if (*p == ' ' && convert)
		{
			++pendingSpaces;
			++column;
			if (column % width == 0)
			{
				// A single space reaching a tab stop stays a space, as in the Windows version.
				out += pendingSpaces > 1 ? std::string("\t") : std::string(" ");
				pendingSpaces = 0;
			}
			continue;
		}
		out.append(static_cast<std::size_t>(pendingSpaces), ' ');
		pendingSpaces = 0;
		if (*p != ' ')
			leading = false;
		out.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
		++column;
	}
	out.append(static_cast<std::size_t>(pendingSpaces), ' ');
	return out;
}

const char *hashName(HashKind kind) noexcept
{
	switch (kind)
	{
		case HashKind::Md5: return "MD5";
		case HashKind::Sha1: return "SHA-1";
		case HashKind::Sha256: return "SHA-256";
		default: return "SHA-512";
	}
}

std::string hashHex(HashKind kind, const std::string &data)
{
	GChecksumType type = G_CHECKSUM_SHA512;
	switch (kind)
	{
		case HashKind::Md5: type = G_CHECKSUM_MD5; break;
		case HashKind::Sha1: type = G_CHECKSUM_SHA1; break;
		case HashKind::Sha256: type = G_CHECKSUM_SHA256; break;
		case HashKind::Sha512: type = G_CHECKSUM_SHA512; break;
	}
	gchar *digest = g_compute_checksum_for_data(type, reinterpret_cast<const guchar *>(data.data()), data.size());
	std::string result = digest ? digest : "";
	g_free(digest);
	return result;
}

std::string expandVariables(const std::string &text, const std::map<std::string, std::string> &values)
{
	std::string out;
	std::size_t position = 0;
	while (position < text.size())
	{
		const auto start = text.find("$(", position);
		if (start == std::string::npos)
			break;
		const auto end = text.find(')', start + 2);
		if (end == std::string::npos)
			break;
		out += text.substr(position, start - position);
		const auto found = values.find(text.substr(start + 2, end - start - 2));
		out += found == values.end() ? text.substr(start, end - start + 1) : found->second;
		position = end + 1;
	}
	out += text.substr(position);
	return out;
}

std::string shellQuote(const std::string &value)
{
	std::string out = "'";
	for (const char c : value)
		out += c == '\'' ? std::string("'\\''") : std::string(1, c);
	return out + "'";
}

const std::vector<EncodingInfo> &encodings()
{
	static const std::vector<EncodingInfo> list = {
		{"UTF-8", "UTF-8", ""},
		{"UTF-8-BOM", "UTF-8 com BOM", ""},
		{"UTF-16LE", "UTF-16 LE com BOM", ""},
		{"UTF-16BE", "UTF-16 BE com BOM", ""},
		{"WINDOWS-1252", "ANSI (Windows-1252)", ""},
		{"ISO-8859-1", "ISO-8859-1 (Latin-1)", "Europa ocidental"},
		{"ISO-8859-15", "ISO-8859-15 (Latin-9)", "Europa ocidental"},
		{"CP850", "OEM 850", "Europa ocidental"},
		{"CP437", "OEM 437 (EUA)", "Europa ocidental"},
		{"WINDOWS-1250", "Windows-1250", "Europa central"},
		{"ISO-8859-2", "ISO-8859-2", "Europa central"},
		{"WINDOWS-1251", "Windows-1251", "Cirílico"},
		{"KOI8-R", "KOI8-R", "Cirílico"},
		{"ISO-8859-5", "ISO-8859-5", "Cirílico"},
		{"WINDOWS-1253", "Windows-1253", "Grego"},
		{"ISO-8859-7", "ISO-8859-7", "Grego"},
		{"WINDOWS-1254", "Windows-1254", "Turco"},
		{"ISO-8859-9", "ISO-8859-9", "Turco"},
		{"WINDOWS-1255", "Windows-1255", "Hebraico"},
		{"WINDOWS-1256", "Windows-1256", "Árabe"},
		{"WINDOWS-1257", "Windows-1257", "Báltico"},
		{"WINDOWS-1258", "Windows-1258", "Vietnamita"},
		{"WINDOWS-874", "Windows-874", "Tailandês"},
		{"SHIFT_JIS", "Shift-JIS", "Japonês"},
		{"EUC-JP", "EUC-JP", "Japonês"},
		{"GB18030", "GB18030", "Chinês simplificado"},
		{"BIG5", "Big5", "Chinês tradicional"},
		{"EUC-KR", "EUC-KR", "Coreano"},
	};
	return list;
}

std::string encodingLabel(const std::string &id)
{
	for (const auto &entry : encodings())
	{
		if (id == entry.id)
			return entry.label;
	}
	return id.empty() ? "UTF-8" : id;
}

bool decodeBytes(const std::string &bytes, const std::string &forced, std::string &encoding, std::string &utf8, std::string &error)
{
	auto convert = [&](const std::string &input, const char *from) -> bool {
		gsize written = 0;
		GError *gerror = nullptr;
		gchar *converted = g_convert(input.data(), static_cast<gssize>(input.size()), "UTF-8", from, nullptr, &written, &gerror);
		if (!converted)
		{
			error = gerror ? gerror->message : "conversão falhou";
			if (gerror)
				g_error_free(gerror);
			return false;
		}
		utf8.assign(converted, written);
		g_free(converted);
		return true;
	};
	const bool utf8Bom = bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0;
	const bool le = bytes.size() >= 2 && bytes.compare(0, 2, "\xFF\xFE") == 0;
	const bool be = bytes.size() >= 2 && bytes.compare(0, 2, "\xFE\xFF") == 0;
	std::string wanted = forced;
	if (wanted.empty())
		wanted = utf8Bom ? "UTF-8-BOM" : le ? "UTF-16LE" : be ? "UTF-16BE"
			: g_utf8_validate(bytes.data(), static_cast<gssize>(bytes.size()), nullptr) ? "UTF-8" : "WINDOWS-1252";
	if (wanted == "UTF-8" || wanted == "UTF-8-BOM")
	{
		const std::string body = utf8Bom ? bytes.substr(3) : bytes;
		if (!g_utf8_validate(body.data(), static_cast<gssize>(body.size()), nullptr))
		{
			error = "O arquivo não é UTF-8 válido.";
			return false;
		}
		utf8 = body;
		encoding = wanted;
		return true;
	}
	if (wanted == "UTF-16LE" || wanted == "UTF-16BE")
	{
		const bool hasBom = wanted == "UTF-16LE" ? le : be;
		if (!convert(hasBom ? bytes.substr(2) : bytes, wanted.c_str()))
			return false;
		encoding = wanted;
		return true;
	}
	if (!convert(bytes, wanted.c_str()))
	{
		// Windows-1252 leaves five bytes undefined; read those files as Latin-1.
		if (!forced.empty() || !convert(bytes, "ISO-8859-1"))
			return false;
		wanted = "ISO-8859-1";
	}
	encoding = wanted;
	return true;
}

bool encodeText(const std::string &utf8, const std::string &encoding, std::string &bytes, std::string &error)
{
	if (encoding.empty() || encoding == "UTF-8")
	{
		bytes = utf8;
		return true;
	}
	if (encoding == "UTF-8-BOM")
	{
		bytes = "\xEF\xBB\xBF" + utf8;
		return true;
	}
	gsize written = 0;
	GError *gerror = nullptr;
	gchar *converted = g_convert(utf8.data(), static_cast<gssize>(utf8.size()), encoding.c_str(), "UTF-8", nullptr, &written, &gerror);
	if (!converted)
	{
		error = "Algum caractere do texto não existe na codificação " + encodingLabel(encoding)
			+ ". Escolha UTF-8 em Codificação ou remova esse caractere."
			+ (gerror ? std::string("\n\n") + gerror->message : std::string());
		if (gerror)
			g_error_free(gerror);
		return false;
	}
	bytes.assign(converted, written);
	g_free(converted);
	if (encoding == "UTF-16LE")
		bytes.insert(0, "\xFF\xFE");
	else if (encoding == "UTF-16BE")
		bytes.insert(0, "\xFE\xFF");
	return true;
}

std::pair<std::string, std::string> blockCommentFor(const std::string &languageId)
{
	static const std::map<std::string, std::pair<std::string, std::string>> delimiters = {
		{"cpp", {"/*", "*/"}}, {"java", {"/*", "*/"}}, {"javascript", {"/*", "*/"}}, {"rust", {"/*", "*/"}},
		{"go", {"/*", "*/"}}, {"php", {"/*", "*/"}}, {"dart", {"/*", "*/"}}, {"css", {"/*", "*/"}},
		{"sql", {"/*", "*/"}}, {"html", {"<!--", "-->"}}, {"markdown", {"<!--", "-->"}},
		{"lua", {"--[[", "]]"}}, {"haskell", {"{-", "-}"}}, {"powershell", {"<#", "#>"}},
		{"python", {"\"\"\"", "\"\"\""}}, {"perl", {"=pod", "=cut"}}, {"ruby", {"=begin", "=end"}}};
	const auto found = delimiters.find(languageId);
	return found == delimiters.end() ? std::pair<std::string, std::string>() : found->second;
}
}
