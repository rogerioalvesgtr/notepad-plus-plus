#include "NppStyles.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>

#include "Settings.h"

namespace NppLinux
{
// Generated at build time from the Windows version's files (see the Makefile).
extern const char nppStylersLight[];
extern const char nppStylersDark[];
extern const char nppLanguages[];

namespace
{
std::string attribute(const std::string &tag, const std::string &name)
{
	const std::string key = " " + name + "=\"";
	const auto start = tag.find(key);
	if (start == std::string::npos)
		return {};
	const auto end = tag.find('"', start + key.size());
	if (end == std::string::npos)
		return {};
	return unescapeXml(tag.substr(start + key.size(), end - start - key.size()));
}

int colourOf(const std::string &hex)
{
	if (hex.size() != 6)
		return -1;
	char *end = nullptr;
	const long value = std::strtol(hex.c_str(), &end, 16);
	if (!end || *end)
		return -1;
	// RRGGBB in the files, BBGGRR in Scintilla.
	return static_cast<int>(((value & 0xFF) << 16) | (value & 0xFF00) | ((value >> 16) & 0xFF));
}

std::map<std::string, NppLexerStyles> parseStylers(const std::string &xml)
{
	std::map<std::string, NppLexerStyles> result;
	int defaultBack = -1;
	const auto global = xml.find("name=\"Default Style\"");
	if (global != std::string::npos)
	{
		const auto tagStart = xml.rfind('<', global);
		const auto tagEnd = xml.find('>', global);
		defaultBack = colourOf(attribute(xml.substr(tagStart, tagEnd - tagStart), "bgColor"));
	}
	std::size_t position = 0;
	while ((position = xml.find("<LexerType ", position)) != std::string::npos)
	{
		const auto end = xml.find("</LexerType>", position);
		if (end == std::string::npos)
			break;
		const auto headerEnd = xml.find('>', position);
		const std::string name = attribute(xml.substr(position, headerEnd - position), "name");
		NppLexerStyles &lexer = result[name];
		lexer.defaultBack = defaultBack;
		std::size_t style = headerEnd;
		while ((style = xml.find("<WordsStyle ", style)) != std::string::npos && style < end)
		{
			const auto close = xml.find('>', style);
			const std::string tag = xml.substr(style, close - style);
			NppStyle entry;
			entry.id = std::atoi(attribute(tag, "styleID").c_str());
			entry.fore = colourOf(attribute(tag, "fgColor"));
			entry.back = colourOf(attribute(tag, "bgColor"));
			const std::string fontStyle = attribute(tag, "fontStyle");
			entry.fontStyle = fontStyle.empty() ? -1 : std::atoi(fontStyle.c_str());
			entry.keywordClass = attribute(tag, "keywordClass");
			if (tag.back() != '/')
			{
				// Extra keywords the person added are the element's text.
				const auto textEnd = xml.find("</WordsStyle>", close);
				if (textEnd != std::string::npos && textEnd < end)
					entry.extraKeywords = unescapeXml(xml.substr(close + 1, textEnd - close - 1));
			}
			lexer.styles.push_back(std::move(entry));
			style = close;
		}
		position = end;
	}
	return result;
}

std::map<std::string, std::map<std::string, std::string>> parseLanguages(const std::string &xml)
{
	std::map<std::string, std::map<std::string, std::string>> result;
	std::size_t position = 0;
	while ((position = xml.find("<Language ", position)) != std::string::npos)
	{
		const auto headerEnd = xml.find('>', position);
		if (headerEnd == std::string::npos)
			break;
		const std::string header = xml.substr(position, headerEnd - position);
		auto &lists = result[attribute(header, "name")];
		if (header.back() == '/')
		{
			position = headerEnd;
			continue;
		}
		const auto end = xml.find("</Language>", headerEnd);
		if (end == std::string::npos)
			break;
		std::size_t keywords = headerEnd;
		while ((keywords = xml.find("<Keywords ", keywords)) != std::string::npos && keywords < end)
		{
			const auto close = xml.find('>', keywords);
			const std::string tag = xml.substr(keywords, close - keywords);
			if (tag.back() == '/')
			{
				keywords = close;
				continue;
			}
			const auto textEnd = xml.find("</Keywords>", close);
			if (textEnd == std::string::npos || textEnd > end)
				break;
			lists[attribute(tag, "name")] = unescapeXml(xml.substr(close + 1, textEnd - close - 1));
			keywords = textEnd;
		}
		position = end;
	}
	return result;
}

std::once_flag loaded;
std::map<std::string, NppLexerStyles> lightStyles;
std::map<std::string, NppLexerStyles> darkStyles;
std::map<std::string, std::map<std::string, std::string>> keywordLists;

void load()
{
	std::call_once(loaded, [] {
		lightStyles = parseStylers(nppStylersLight);
		darkStyles = parseStylers(nppStylersDark);
		keywordLists = parseLanguages(nppLanguages);
	});
}

std::string extension(const std::string &fileName)
{
	const auto dot = fileName.rfind('.');
	if (dot == std::string::npos)
		return {};
	std::string result = fileName.substr(dot + 1);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return result;
}
}

const NppLexerStyles *nppLexerStyles(const std::string &styler, bool dark)
{
	load();
	const auto &table = dark ? darkStyles : lightStyles;
	const auto found = table.find(styler);
	return found == table.end() ? nullptr : &found->second;
}

std::string nppKeywords(const std::string &language, const std::string &list)
{
	load();
	const auto found = keywordLists.find(language);
	if (found == keywordLists.end())
		return {};
	const auto words = found->second.find(list);
	return words == found->second.end() ? std::string() : words->second;
}

NppLanguageSpec nppLanguageSpec(const std::string &languageId, const std::string &fileName)
{
	const std::string ext = extension(fileName);
	NppLanguageSpec spec;
	// Keyword sets as the Windows version passes them to each lexer.
	auto cppFamily = [&](const std::string &name) {
		spec.styler = name;
		spec.language = name;
		spec.keywordSets = {{name, {"instre1", 0}}, {name, {"type1", 1}}, {"cpp", {"type2", 2}}, {name, {"instre2", 3}}};
	};
	auto generic = [&](const std::string &name, std::initializer_list<int> lists) {
		static const char *names[] = {"instre1", "instre2", "type1", "type2", "type3", "type4", "type5", "type6", "type7"};
		spec.styler = name;
		spec.language = name;
		for (const int list : lists)
			spec.keywordSets.push_back({name, {names[list], list}});
	};
	if (languageId == "cpp")
		cppFamily(ext == "c" ? "c" : "cpp");
	else if (languageId == "java")
		cppFamily(ext == "cs" ? "cs" : "java");
	else if (languageId == "go")
		cppFamily("go");
	else if (languageId == "javascript")
	{
		const bool typescript = ext == "ts" || ext == "tsx" || ext == "mts" || ext == "cts";
		if (typescript)
		{
			spec.styler = spec.language = "typescript";
			spec.keywordSets = {{"typescript", {"instre1", 0}}, {"typescript", {"type1", 1}}, {"cpp", {"type2", 2}}};
		}
		else
			cppFamily("javascript.js");
		spec.properties = {{"lexer.cpp.backquoted.strings", "1"}, {"lexer.cpp.track.preprocessor", "0"}};
	}
	else if (languageId == "dart")
	{
		// Not in the Windows version: C++ colours with Dart's own keywords.
		spec.styler = "cpp";
	}
	else if (languageId == "python")
	{
		generic("python", {0, 1});
		spec.properties = {{"lexer.python.decorator.attributes", "1"}, {"lexer.python.identifier.attributes", "1"}};
	}
	else if (languageId == "rust")
		generic("rust", {0, 1, 2, 3, 4, 5, 6});
	else if (languageId == "shell")
		generic("bash", {0});
	else if (languageId == "html")
	{
		const bool xml = ext == "xml" || ext == "svg" || ext == "xsl" || ext == "xslt" || ext == "xsd" || ext == "plist" || ext == "rss" || ext == "xaml";
		if (xml)
		{
			spec.styler = spec.language = "xml";
			spec.keywordSets = {{"xml", {"instre1", 5}}};
			spec.properties = {{"lexer.xml.allow.scripts", "0"}};
		}
		else
		{
			spec.styler = spec.language = "html";
			spec.keywordSets = {{"html", {"instre1", 0}}, {"javascript", {"instre1", 1}}, {"php", {"instre1", 4}}, {"html", {"instre2", 5}}};
		}
	}
	else if (languageId == "php")
	{
		spec.styler = spec.language = "php";
		spec.keywordSets = {{"html", {"instre1", 0}}, {"javascript", {"instre1", 1}}, {"php", {"instre1", 4}}};
	}
	else if (languageId == "css")
		generic("css", {0, 1, 4, 6});
	else if (languageId == "json")
		generic("json", {0, 1});
	else if (languageId == "yaml")
		generic("yaml", {0});
	else if (languageId == "toml")
		spec.styler = "ini"; // the props lexer, as for INI files in the Windows version
	else if (languageId == "sql")
		generic("sql", {0, 1, 4});
	else if (languageId == "lua")
		generic("lua", {0, 1, 2, 3, 4, 5, 6, 7});
	else if (languageId == "perl")
		generic("perl", {0});
	else if (languageId == "ruby")
		generic("ruby", {0});
	else if (languageId == "make")
		spec.styler = "makefile";
	else if (languageId == "cmake")
		generic("cmake", {0, 1, 2});
	else if (languageId == "diff")
		spec.styler = "diff";
	else if (languageId == "tex")
		spec.styler = "latex";
	else if (languageId == "powershell")
		generic("powershell", {0, 1, 2, 3, 4, 5});
	else if (languageId == "batch")
		generic("batch", {0});
	else if (languageId == "nsis")
		generic("nsis", {0, 1, 2, 3});
	else if (languageId == "vb")
		generic("vb", {0});
	else if (languageId == "haskell")
		generic("haskell", {0});
	return spec;
}
}
