#include "Theme.h"

#include <cstdio>
#include <sstream>
#include <vector>

namespace NppLinux
{
namespace
{
struct ItemInfo
{
	const char *key;
	const char *label;
	bool foreground;
	bool background;
	bool fontStyle;
};

const ItemInfo &info(ThemeItem item)
{
	static const ItemInfo items[themeItemCount] = {
		{"text", "Texto padrão", true, true, true},
		{"comment", "Comentários", true, false, true},
		{"keyword", "Palavras-chave", true, false, true},
		{"string", "Textos entre aspas", true, false, true},
		{"number", "Números", true, false, true},
		{"operator", "Operadores", true, false, true},
		{"preprocessor", "Pré-processador e marcações", true, false, true},
		{"linenumber", "Números das linhas", true, true, false},
		{"caretline", "Linha atual", false, true, false},
		{"selection", "Seleção", true, true, false},
		{"brace", "Par de chaves destacado", true, false, true},
	};
	return items[static_cast<std::size_t>(item)];
}

std::vector<std::string> split(const std::string &text, char separator)
{
	std::vector<std::string> parts;
	std::string part;
	std::istringstream stream(text);
	while (std::getline(stream, part, separator))
		parts.push_back(part);
	return parts;
}
}

int rgb(int red, int green, int blue)
{
	return (red & 0xFF) | ((green & 0xFF) << 8) | ((blue & 0xFF) << 16);
}

std::string colourToHex(int colour)
{
	if (colour < 0)
		return "-";
	char buffer[8];
	std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X", colour & 0xFF, (colour >> 8) & 0xFF, (colour >> 16) & 0xFF);
	return buffer;
}

int colourFromHex(const std::string &text, int fallback)
{
	std::string value = text;
	if (!value.empty() && value.front() == '#')
		value.erase(0, 1);
	if (value.size() != 6 || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
		return fallback;
	const unsigned long parsed = std::stoul(value, nullptr, 16);
	return rgb(static_cast<int>((parsed >> 16) & 0xFF), static_cast<int>((parsed >> 8) & 0xFF), static_cast<int>(parsed & 0xFF));
}

const char *themeItemKey(ThemeItem item)
{
	return info(item).key;
}

const char *themeItemLabel(ThemeItem item)
{
	return info(item).label;
}

bool themeItemHasForeground(ThemeItem item)
{
	return info(item).foreground;
}

bool themeItemHasBackground(ThemeItem item)
{
	return info(item).background;
}

bool themeItemHasFontStyle(ThemeItem item)
{
	return info(item).fontStyle;
}

Theme defaultTheme(bool dark)
{
	Theme theme;
	auto set = [&](ThemeItem item, int fore, int back = -1, bool bold = false, bool italic = false) {
		theme[item] = {fore, back, bold, italic};
	};
	if (dark)
	{
		set(ThemeItem::Text, rgb(220, 220, 220), rgb(30, 30, 30));
		set(ThemeItem::Comment, rgb(106, 153, 85), -1, false, true);
		set(ThemeItem::Keyword, rgb(86, 156, 214), -1, true);
		set(ThemeItem::String, rgb(206, 145, 114));
		set(ThemeItem::Number, rgb(181, 206, 145));
		set(ThemeItem::Operator, rgb(212, 212, 212));
		set(ThemeItem::Preprocessor, rgb(197, 134, 192));
		set(ThemeItem::LineNumber, rgb(133, 133, 133), rgb(37, 37, 37));
		set(ThemeItem::CaretLine, -1, rgb(42, 42, 42));
		set(ThemeItem::Selection, rgb(255, 255, 255), rgb(38, 79, 120));
		set(ThemeItem::BraceMatch, rgb(255, 215, 0), -1, true);
	}
	else
	{
		set(ThemeItem::Text, rgb(32, 32, 32), rgb(255, 255, 255));
		set(ThemeItem::Comment, rgb(0, 128, 0), -1, false, true);
		set(ThemeItem::Keyword, rgb(0, 0, 153), -1, true);
		set(ThemeItem::String, rgb(163, 69, 21));
		set(ThemeItem::Number, rgb(181, 98, 0));
		set(ThemeItem::Operator, rgb(120, 0, 120));
		set(ThemeItem::Preprocessor, rgb(128, 64, 0));
		set(ThemeItem::LineNumber, rgb(100, 100, 100), rgb(243, 243, 243));
		set(ThemeItem::CaretLine, -1, rgb(242, 246, 250));
		set(ThemeItem::Selection, rgb(0, 0, 0), rgb(173, 214, 255));
		set(ThemeItem::BraceMatch, rgb(220, 0, 0), -1, true);
	}
	return theme;
}

std::string serialiseTheme(const Theme &theme)
{
	std::string text;
	for (std::size_t i = 0; i < themeItemCount; ++i)
	{
		const auto item = static_cast<ThemeItem>(i);
		const StyleSpec &spec = theme[item];
		if (!text.empty())
			text += ';';
		text += std::string(themeItemKey(item)) + "=" + colourToHex(spec.fore) + "/" + colourToHex(spec.back) + "/"
			+ (spec.bold ? "1" : "0") + "/" + (spec.italic ? "1" : "0");
	}
	return text;
}

Theme parseTheme(const std::string &text, bool dark)
{
	Theme theme = defaultTheme(dark);
	for (const auto &entry : split(text, ';'))
	{
		const auto equals = entry.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = entry.substr(0, equals);
		const auto values = split(entry.substr(equals + 1), '/');
		if (values.size() != 4)
			continue;
		for (std::size_t i = 0; i < themeItemCount; ++i)
		{
			const auto item = static_cast<ThemeItem>(i);
			if (key != themeItemKey(item))
				continue;
			StyleSpec &spec = theme[item];
			if (themeItemHasForeground(item))
				spec.fore = colourFromHex(values[0], spec.fore);
			if (themeItemHasBackground(item))
				spec.back = colourFromHex(values[1], spec.back);
			if (themeItemHasFontStyle(item))
			{
				spec.bold = values[2] == "1";
				spec.italic = values[3] == "1";
			}
		}
	}
	return theme;
}
}
