#ifndef NPP_LINUX_THEME_H
#define NPP_LINUX_THEME_H

#include <array>
#include <cstddef>
#include <string>

namespace NppLinux
{
enum class ThemeItem
{
	Text,
	Comment,
	Keyword,
	String,
	Number,
	Operator,
	Preprocessor,
	LineNumber,
	CaretLine,
	Selection,
	BraceMatch,
	Count
};

constexpr std::size_t themeItemCount = static_cast<std::size_t>(ThemeItem::Count);

struct StyleSpec
{
	int fore = -1; // 0xBBGGRR as Scintilla expects; -1 when the item has no foreground
	int back = -1; // -1 when the item uses the text background
	bool bold = false;
	bool italic = false;
};

struct Theme
{
	std::array<StyleSpec, themeItemCount> items;

	StyleSpec &operator[](ThemeItem item) { return items[static_cast<std::size_t>(item)]; }
	const StyleSpec &operator[](ThemeItem item) const { return items[static_cast<std::size_t>(item)]; }
};

const char *themeItemKey(ThemeItem item);
const char *themeItemLabel(ThemeItem item);
bool themeItemHasForeground(ThemeItem item);
bool themeItemHasBackground(ThemeItem item);
bool themeItemHasFontStyle(ThemeItem item);

Theme defaultTheme(bool dark);
// Settings store a compact text such as "comment=#008000/-/0/1;keyword=..." (fore/back/bold/italic).
std::string serialiseTheme(const Theme &theme);
Theme parseTheme(const std::string &text, bool dark);

int rgb(int red, int green, int blue);
std::string colourToHex(int colour);
int colourFromHex(const std::string &text, int fallback);
}

#endif
