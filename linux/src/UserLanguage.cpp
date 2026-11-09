#include "UserLanguage.h"

#include "EditorDocument.h"
#include "Settings.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include "Scintilla.h"

namespace NppLinux
{
namespace
{
constexpr const char *endOfLine = "((EOL))";

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
		return c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
	});
	return text;
}

std::string attribute(const std::string &tag, const std::string &name)
{
	const std::string marker = " " + name + "=\"";
	auto position = tag.find(marker);
	if (position == std::string::npos)
	{
		const std::string tabbed = "\t" + name + "=\"";
		position = tag.find(tabbed);
		if (position == std::string::npos)
			return {};
	}
	position += marker.size();
	const auto end = tag.find('"', position);
	return end == std::string::npos ? std::string() : unescapeXml(tag.substr(position, end - position));
}

// Calls back with (opening tag text, inner text) for every <name ...>...</name> or <name .../>.
template <typename Callback>
void forEachElement(const std::string &xml, const std::string &name, Callback callback)
{
	const std::string open = "<" + name;
	const std::string close = "</" + name + ">";
	std::size_t position = 0;
	while ((position = xml.find(open, position)) != std::string::npos)
	{
		const char next = position + open.size() < xml.size() ? xml[position + open.size()] : '\0';
		if (next != ' ' && next != '\t' && next != '\n' && next != '\r' && next != '>' && next != '/')
		{
			position += open.size();
			continue;
		}
		const auto tagEnd = xml.find('>', position);
		if (tagEnd == std::string::npos)
			return;
		const std::string tag = xml.substr(position, tagEnd - position + 1);
		std::string inner;
		std::size_t after = tagEnd + 1;
		if (tag.size() < 2 || tag[tag.size() - 2] != '/')
		{
			const auto end = xml.find(close, tagEnd);
			if (end == std::string::npos)
				return;
			inner = xml.substr(tagEnd + 1, end - tagEnd - 1);
			after = end + close.size();
		}
		callback(tag, inner);
		position = after;
	}
}

// "00// 01 03/*" -> code/value pairs.
std::vector<std::pair<int, std::string>> codedValues(const std::string &text)
{
	std::vector<std::pair<int, std::string>> values;
	for (const auto &token : splitWords(text))
	{
		if (token.size() < 2 || !std::isdigit(static_cast<unsigned char>(token[0])) || !std::isdigit(static_cast<unsigned char>(token[1])))
			continue;
		values.emplace_back((token[0] - '0') * 10 + (token[1] - '0'), token.substr(2));
	}
	return values;
}

int styleForName(const std::string &name)
{
	if (name == "DEFAULT")
		return UserDefault;
	if (name == "COMMENTS")
		return UserComment;
	if (name == "LINE COMMENTS")
		return UserLineComment;
	if (name == "NUMBERS")
		return UserNumber;
	if (name == "OPERATORS")
		return UserOperator;
	if (name.rfind("KEYWORDS", 0) == 0 && name.size() == 9 && name[8] >= '1' && name[8] <= '8')
		return UserKeyword1 + (name[8] - '1');
	if (name.rfind("DELIMITERS", 0) == 0 && name.size() == 11 && name[10] >= '1' && name[10] <= '8')
		return UserDelimiter1 + (name[10] - '1');
	return -1;
}

std::string styleName(int style)
{
	switch (style)
	{
		case UserDefault: return "DEFAULT";
		case UserComment: return "COMMENTS";
		case UserLineComment: return "LINE COMMENTS";
		case UserNumber: return "NUMBERS";
		case UserOperator: return "OPERATORS";
		default: break;
	}
	if (style >= UserKeyword1 && style < UserKeyword1 + 8)
		return "KEYWORDS" + std::to_string(style - UserKeyword1 + 1);
	if (style >= UserDelimiter1 && style < UserDelimiter1 + 8)
		return "DELIMITERS" + std::to_string(style - UserDelimiter1 + 1);
	return {};
}

std::string hexWithoutHash(int colour)
{
	const std::string hex = colourToHex(colour);
	return hex.size() == 7 ? hex.substr(1) : std::string("000000");
}

bool parseOne(const std::string &tag, const std::string &body, UserLanguage &language)
{
	language.name = attribute(tag, "name");
	if (language.name.empty())
		return false;
	language.extensions.clear();
	for (auto &extension : splitWords(attribute(tag, "ext")))
	{
		while (!extension.empty() && extension.front() == '.')
			extension.erase(0, 1);
		if (!extension.empty())
			language.extensions.push_back(lower(extension));
	}
	forEachElement(body, "Global", [&](const std::string &global, const std::string &) {
		language.caseIgnored = attribute(global, "caseIgnored") == "yes";
	});
	forEachElement(body, "Keywords", [&](const std::string &keywordTag, const std::string &inner) {
		const std::string name = attribute(keywordTag, "name");
		const std::string text = unescapeXml(inner);
		if (name == "Comments")
		{
			for (const auto &[code, value] : codedValues(text))
			{
				if (code == 0 && !value.empty())
					language.lineComments.push_back(value);
				else if (code == 3 && language.blockOpen.empty())
					language.blockOpen = value;
				else if (code == 4 && language.blockClose.empty())
					language.blockClose = value;
			}
		}
		else if (name == "Operators1")
			language.operators = splitWords(text);
		else if (name == "Delimiters")
		{
			for (const auto &[code, value] : codedValues(text))
			{
				if (code >= 24 || value.empty())
					continue;
				UserDelimiter &delimiter = language.delimiters[static_cast<std::size_t>(code / 3)];
				std::string &slot = code % 3 == 0 ? delimiter.open : code % 3 == 1 ? delimiter.escape : delimiter.close;
				if (slot.empty())
					slot = value;
			}
		}
		else if (name.rfind("Keywords", 0) == 0 && name.size() == 9 && name[8] >= '1' && name[8] <= '8')
			language.keywords[static_cast<std::size_t>(name[8] - '1')] = splitWords(text);
	});
	forEachElement(body, "WordsStyle", [&](const std::string &styleTag, const std::string &) {
		const int style = styleForName(attribute(styleTag, "name"));
		if (style < 0)
			return;
		StyleSpec spec;
		const std::string fore = attribute(styleTag, "fgColor");
		const std::string back = attribute(styleTag, "bgColor");
		spec.fore = fore.empty() ? -1 : colourFromHex(fore, -1);
		spec.back = back.empty() ? -1 : colourFromHex(back, -1);
		const std::string fontStyle = attribute(styleTag, "fontStyle");
		const int flags = fontStyle.empty() ? 0 : std::atoi(fontStyle.c_str());
		spec.bold = (flags & 1) != 0;
		spec.italic = (flags & 2) != 0;
		language.styles[style] = spec;
	});
	for (auto &delimiter : language.delimiters)
	{
		if (!delimiter.open.empty() && delimiter.close.empty())
			delimiter.close = delimiter.open;
	}
	return true;
}
}

std::vector<std::string> splitWords(const std::string &text)
{
	std::vector<std::string> words;
	std::istringstream stream(text);
	std::string word;
	while (stream >> word)
		words.push_back(word);
	return words;
}

std::string joinWords(const std::vector<std::string> &words)
{
	std::string text;
	for (const auto &word : words)
	{
		if (!text.empty())
			text += ' ';
		text += word;
	}
	return text;
}

bool parseUserLanguages(const std::string &xml, std::vector<UserLanguage> &languages, std::string &error)
{
	std::size_t found = 0;
	forEachElement(xml, "UserLang", [&](const std::string &tag, const std::string &body) {
		UserLanguage language;
		if (parseOne(tag, body, language))
		{
			languages.push_back(std::move(language));
			++found;
		}
	});
	if (!found)
		error = "Nenhuma linguagem definida pelo usuário (elemento UserLang) foi encontrada.";
	return found > 0;
}

std::string userLanguageToXml(const UserLanguage &language)
{
	std::ostringstream xml;
	xml << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<NotepadPlus>\n";
	xml << "    <UserLang name=\"" << escapeXml(language.name) << "\" ext=\"" << escapeXml(joinWords(language.extensions)) << "\" udlVersion=\"2.1\">\n";
	xml << "        <Settings>\n";
	xml << "            <Global caseIgnored=\"" << (language.caseIgnored ? "yes" : "no") << "\" allowFoldOfComments=\"no\" foldCompact=\"no\" forcePureLC=\"0\" decimalSeparator=\"0\" />\n";
	xml << "            <Prefix Keywords1=\"no\" Keywords2=\"no\" Keywords3=\"no\" Keywords4=\"no\" Keywords5=\"no\" Keywords6=\"no\" Keywords7=\"no\" Keywords8=\"no\" />\n";
	xml << "        </Settings>\n        <KeywordLists>\n";
	std::string comments;
	for (const auto &comment : language.lineComments)
		comments += "00" + comment + " ";
	comments += "01 02 03" + language.blockOpen + " 04" + language.blockClose;
	xml << "            <Keywords name=\"Comments\">" << escapeXml(comments) << "</Keywords>\n";
	for (const char *name : {"Numbers, prefix1", "Numbers, prefix2", "Numbers, extras1", "Numbers, extras2", "Numbers, suffix1", "Numbers, suffix2", "Numbers, range"})
		xml << "            <Keywords name=\"" << name << "\"></Keywords>\n";
	xml << "            <Keywords name=\"Operators1\">" << escapeXml(joinWords(language.operators)) << "</Keywords>\n";
	xml << "            <Keywords name=\"Operators2\"></Keywords>\n";
	for (const char *name : {"Folders in code1, open", "Folders in code1, middle", "Folders in code1, close", "Folders in code2, open",
			"Folders in code2, middle", "Folders in code2, close", "Folders in comment, open", "Folders in comment, middle", "Folders in comment, close"})
		xml << "            <Keywords name=\"" << name << "\"></Keywords>\n";
	for (std::size_t i = 0; i < language.keywords.size(); ++i)
		xml << "            <Keywords name=\"Keywords" << (i + 1) << "\">" << escapeXml(joinWords(language.keywords[i])) << "</Keywords>\n";
	std::string delimiters;
	for (std::size_t i = 0; i < language.delimiters.size(); ++i)
	{
		const auto &delimiter = language.delimiters[i];
		char code[3];
		for (int part = 0; part < 3; ++part)
		{
			std::snprintf(code, sizeof code, "%02d", static_cast<int>(i * 3 + part));
			if (!delimiters.empty())
				delimiters += ' ';
			delimiters += code;
			delimiters += part == 0 ? delimiter.open : part == 1 ? delimiter.escape : delimiter.close;
		}
	}
	xml << "            <Keywords name=\"Delimiters\">" << escapeXml(delimiters) << "</Keywords>\n";
	xml << "        </KeywordLists>\n        <Styles>\n";
	for (const auto &[style, spec] : language.styles)
	{
		const std::string name = styleName(style);
		if (name.empty())
			continue;
		xml << "            <WordsStyle name=\"" << name << "\" fgColor=\"" << hexWithoutHash(spec.fore < 0 ? 0 : spec.fore)
			<< "\" bgColor=\"" << hexWithoutHash(spec.back < 0 ? 0xFFFFFF : spec.back) << "\" colorStyle=\"" << (spec.back < 0 ? 1 : 3)
			<< "\" fontStyle=\"" << ((spec.bold ? 1 : 0) | (spec.italic ? 2 : 0)) << "\" nesting=\"0\" />\n";
	}
	xml << "        </Styles>\n    </UserLang>\n</NotepadPlus>\n";
	return xml.str();
}

std::string userLanguageFileName(const std::string &name)
{
	std::string file;
	for (const unsigned char c : name)
		file += (std::isalnum(c) || c == '-' || c == '_' || c >= 0x80) ? static_cast<char>(c) : '_';
	if (file.empty())
		file = "linguagem";
	return file + ".xml";
}

std::vector<UserLanguage> loadUserLanguages(const std::filesystem::path &directory, std::vector<std::string> &errors)
{
	std::vector<UserLanguage> languages;
	std::error_code code;
	if (!std::filesystem::is_directory(directory, code))
		return languages;
	std::vector<std::filesystem::path> files;
	for (std::filesystem::directory_iterator iterator(directory, code); !code && iterator != std::filesystem::directory_iterator(); iterator.increment(code))
	{
		if (iterator->is_regular_file(code) && iterator->path().extension() == ".xml")
			files.push_back(iterator->path());
	}
	std::sort(files.begin(), files.end());
	std::set<std::string> names;
	for (const auto &file : files)
	{
		std::ifstream input(file, std::ios::binary);
		std::ostringstream buffer;
		buffer << input.rdbuf();
		std::vector<UserLanguage> found;
		std::string error;
		if (!parseUserLanguages(buffer.str(), found, error))
		{
			errors.push_back(file.filename().string() + ": " + error);
			continue;
		}
		for (auto &language : found)
		{
			if (!names.insert(lower(language.name)).second)
				continue;
			language.file = file;
			languages.push_back(std::move(language));
		}
	}
	return languages;
}

bool saveUserLanguage(const std::filesystem::path &directory, UserLanguage &language, std::string &error)
{
	std::error_code code;
	std::filesystem::create_directories(directory, code);
	if (code)
	{
		error = code.message();
		return false;
	}
	const std::filesystem::path target = !language.file.empty() && language.file.parent_path() == directory
		? language.file : directory / userLanguageFileName(language.name);
	const std::filesystem::path temporary = target.string() + ".tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		output << userLanguageToXml(language);
		output.close();
		if (!output)
		{
			error = "Não foi possível gravar " + target.string();
			std::filesystem::remove(temporary, code);
			return false;
		}
	}
	std::filesystem::rename(temporary, target, code);
	if (code)
	{
		error = code.message();
		return false;
	}
	language.file = target;
	return true;
}

void styleUserLanguage(EditorDocument &document, const UserLanguage &language, long start, long end)
{
	const long length = static_cast<long>(document.send(SCI_GETLENGTH));
	end = std::min(end, length);
	sptr_t line = document.send(SCI_LINEFROMPOSITION, start);
	const long from = static_cast<long>(document.send(SCI_POSITIONFROMLINE, line));
	const long to = std::min(static_cast<long>(document.send(SCI_GETLINEENDPOSITION, document.send(SCI_LINEFROMPOSITION, end))) + 2, length);
	if (to <= from)
		return;
	std::string text(static_cast<std::size_t>(to - from) + 1, '\0');
	Sci_TextRangeFull range{{from, to}, text.data()};
	document.send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&range));
	text.resize(static_cast<std::size_t>(to - from));
	std::string styles(text.size(), static_cast<char>(UserDefault));

	std::array<std::unordered_set<std::string>, 8> keywordSets;
	for (std::size_t group = 0; group < 8; ++group)
	{
		for (const auto &word : language.keywords[group])
			keywordSets[group].insert(language.caseIgnored ? lower(word) : word);
	}
	std::vector<std::string> operators = language.operators;
	std::sort(operators.begin(), operators.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
	auto matchAt = [&](std::size_t index, const std::string &token) {
		return !token.empty() && text.compare(index, token.size(), token) == 0;
	};
	auto mark = [&](std::size_t index, std::size_t count, int style) {
		for (std::size_t i = index; i < std::min(index + count, styles.size()); ++i)
			styles[i] = static_cast<char>(style);
	};
	auto isWord = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };

	int state = line > 0 ? static_cast<int>(document.send(SCI_GETLINESTATE, static_cast<uptr_t>(line - 1))) : 0;
	std::size_t i = 0;
	while (i < text.size())
	{
		const char c = text[i];
		if (c == '\n')
		{
			const auto &delimiter = state >= 2 && state < 10 ? language.delimiters[static_cast<std::size_t>(state - 2)] : UserDelimiter();
			if (state >= 2 && delimiter.close == endOfLine)
				state = 0;
			document.send(SCI_SETLINESTATE, static_cast<uptr_t>(line++), state);
			++i;
			continue;
		}
		if (state == 1)
		{
			if (matchAt(i, language.blockClose))
			{
				mark(i, language.blockClose.size(), UserComment);
				i += language.blockClose.size();
				state = 0;
			}
			else
				mark(i++, 1, UserComment);
			continue;
		}
		if (state >= 2 && state < 10)
		{
			const auto &delimiter = language.delimiters[static_cast<std::size_t>(state - 2)];
			const int style = UserDelimiter1 + state - 2;
			if (matchAt(i, delimiter.escape))
			{
				mark(i, delimiter.escape.size() + 1, style);
				i += delimiter.escape.size() + 1;
			}
			else if (delimiter.close != endOfLine && matchAt(i, delimiter.close))
			{
				mark(i, delimiter.close.size(), style);
				i += delimiter.close.size();
				state = 0;
			}
			else if (c == '\r' && delimiter.close == endOfLine)
				++i;
			else
				mark(i++, 1, style);
			continue;
		}
		if (matchAt(i, language.blockOpen) && !language.blockClose.empty())
		{
			mark(i, language.blockOpen.size(), UserComment);
			i += language.blockOpen.size();
			state = 1;
			continue;
		}
		const auto comment = std::find_if(language.lineComments.begin(), language.lineComments.end(), [&](const std::string &token) { return matchAt(i, token); });
		if (comment != language.lineComments.end())
		{
			std::size_t lineEnd = text.find('\n', i);
			if (lineEnd == std::string::npos)
				lineEnd = text.size();
			if (lineEnd > i && text[lineEnd - 1] == '\r')
				--lineEnd;
			mark(i, lineEnd - i, UserLineComment);
			i = lineEnd;
			continue;
		}
		bool opened = false;
		for (std::size_t k = 0; k < language.delimiters.size(); ++k)
		{
			if (matchAt(i, language.delimiters[k].open))
			{
				mark(i, language.delimiters[k].open.size(), UserDelimiter1 + static_cast<int>(k));
				i += language.delimiters[k].open.size();
				state = 2 + static_cast<int>(k);
				opened = true;
				break;
			}
		}
		if (opened)
			continue;
		const auto op = std::find_if(operators.begin(), operators.end(), [&](const std::string &token) { return matchAt(i, token); });
		if (op != operators.end())
		{
			mark(i, op->size(), UserOperator);
			i += op->size();
			continue;
		}
		if (isWord(static_cast<unsigned char>(c)))
		{
			std::size_t wordEnd = i;
			while (wordEnd < text.size() && isWord(static_cast<unsigned char>(text[wordEnd])))
				++wordEnd;
			const std::string word = text.substr(i, wordEnd - i);
			if (std::isdigit(static_cast<unsigned char>(word.front())))
				mark(i, word.size(), UserNumber);
			else
			{
				const std::string key = language.caseIgnored ? lower(word) : word;
				for (std::size_t group = 0; group < 8; ++group)
				{
					if (keywordSets[group].count(key))
					{
						mark(i, word.size(), UserKeyword1 + static_cast<int>(group));
						break;
					}
				}
			}
			i = wordEnd;
			continue;
		}
		++i;
	}
	document.send(SCI_SETLINESTATE, static_cast<uptr_t>(line), state);
	document.send(SCI_STARTSTYLING, from, 0);
	document.send(SCI_SETSTYLINGEX, styles.size(), reinterpret_cast<sptr_t>(styles.data()));
}
}
