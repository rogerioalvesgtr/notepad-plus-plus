#include "LineTools.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include <glib.h>

#include "Scintilla.h"

namespace NppLinux
{
namespace
{
struct LineBlock
{
	sptr_t firstLine = 0;
	sptr_t lastLine = 0;
	sptr_t start = 0;
	sptr_t end = 0;
	std::string eol = "\n";
	std::vector<std::string> lines;
};

std::string textRange(EditorDocument &document, sptr_t start, sptr_t end)
{
	std::string text(static_cast<std::size_t>(end - start) + 1, '\0');
	Sci_TextRangeFull range{{start, end}, text.data()};
	document.send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&range));
	text.resize(static_cast<std::size_t>(end - start));
	return text;
}

LineBlock readBlock(EditorDocument &document, bool wholeDocumentWhenEmpty)
{
	LineBlock block;
	const sptr_t selectionStart = document.send(SCI_GETSELECTIONSTART);
	const sptr_t selectionEnd = document.send(SCI_GETSELECTIONEND);
	if (selectionStart == selectionEnd && wholeDocumentWhenEmpty)
	{
		block.firstLine = 0;
		block.lastLine = document.send(SCI_GETLINECOUNT) - 1;
		// The empty line after a final line break is not part of the text to sort.
		if (block.lastLine > 0 && document.send(SCI_POSITIONFROMLINE, block.lastLine) == document.send(SCI_GETLENGTH))
			--block.lastLine;
	}
	else
	{
		block.firstLine = document.send(SCI_LINEFROMPOSITION, selectionStart);
		block.lastLine = document.send(SCI_LINEFROMPOSITION, selectionEnd);
		// A selection ending at the start of a line does not include that line.
		if (block.lastLine > block.firstLine && document.send(SCI_POSITIONFROMLINE, block.lastLine) == selectionEnd)
			--block.lastLine;
	}
	block.start = document.send(SCI_POSITIONFROMLINE, block.firstLine);
	block.end = document.send(SCI_GETLINEENDPOSITION, block.lastLine);
	const int mode = static_cast<int>(document.send(SCI_GETEOLMODE));
	block.eol = mode == SC_EOL_CRLF ? "\r\n" : mode == SC_EOL_CR ? "\r" : "\n";
	for (sptr_t line = block.firstLine; line <= block.lastLine; ++line)
		block.lines.push_back(textRange(document, document.send(SCI_POSITIONFROMLINE, line), document.send(SCI_GETLINEENDPOSITION, line)));
	return block;
}

void writeBlock(EditorDocument &document, const LineBlock &block)
{
	std::string text;
	for (std::size_t i = 0; i < block.lines.size(); ++i)
	{
		if (i)
			text += block.eol;
		text += block.lines[i];
	}
	document.send(SCI_BEGINUNDOACTION);
	document.send(SCI_SETTARGETRANGE, block.start, block.end);
	document.send(SCI_REPLACETARGET, text.size(), reinterpret_cast<sptr_t>(text.c_str()));
	document.send(SCI_SETSEL, block.start, block.start + static_cast<sptr_t>(text.size()));
	document.send(SCI_ENDUNDOACTION);
}

std::string sortKey(const std::string &line, bool ignoreCase)
{
	if (!g_utf8_validate(line.c_str(), static_cast<gssize>(line.size()), nullptr))
		return line;
	gchar *folded = ignoreCase ? g_utf8_casefold(line.c_str(), -1) : g_strdup(line.c_str());
	gchar *key = g_utf8_collate_key(folded, -1);
	std::string result = key ? key : line;
	g_free(key);
	g_free(folded);
	return result;
}
}

bool sortLines(EditorDocument &document, bool descending, bool ignoreCase)
{
	LineBlock block = readBlock(document, true);
	if (block.lines.size() < 2)
		return false;
	std::vector<std::pair<std::string, std::string>> keyed;
	for (auto &line : block.lines)
		keyed.emplace_back(sortKey(line, ignoreCase), std::move(line));
	std::stable_sort(keyed.begin(), keyed.end(), [&](const auto &a, const auto &b) {
		return descending ? b.first < a.first : a.first < b.first;
	});
	block.lines.clear();
	for (auto &entry : keyed)
		block.lines.push_back(std::move(entry.second));
	writeBlock(document, block);
	return true;
}

std::size_t removeDuplicateLines(EditorDocument &document)
{
	LineBlock block = readBlock(document, true);
	std::set<std::string> seen;
	std::vector<std::string> unique;
	for (auto &line : block.lines)
	{
		if (seen.insert(line).second)
			unique.push_back(std::move(line));
	}
	const std::size_t removed = block.lines.size() - unique.size();
	if (removed)
	{
		block.lines = std::move(unique);
		writeBlock(document, block);
	}
	return removed;
}

std::size_t trimTrailingWhitespace(EditorDocument &document)
{
	LineBlock block = readBlock(document, true);
	std::size_t changed = 0;
	for (auto &line : block.lines)
	{
		const auto last = line.find_last_not_of(" \t");
		const std::size_t keep = last == std::string::npos ? 0 : last + 1;
		if (keep != line.size())
		{
			line.resize(keep);
			++changed;
		}
	}
	if (changed)
		writeBlock(document, block);
	return changed;
}

bool toggleLineComment(EditorDocument &document, const std::string &prefix)
{
	if (prefix.empty())
		return false;
	LineBlock block = readBlock(document, false);
	bool allCommented = true;
	bool anyText = false;
	for (const auto &line : block.lines)
	{
		const auto first = line.find_first_not_of(" \t");
		if (first == std::string::npos)
			continue;
		anyText = true;
		if (line.compare(first, prefix.size(), prefix) != 0)
			allCommented = false;
	}
	if (!anyText)
		return false;
	for (auto &line : block.lines)
	{
		const auto first = line.find_first_not_of(" \t");
		if (first == std::string::npos)
			continue;
		if (allCommented)
		{
			std::size_t remove = prefix.size();
			if (first + remove < line.size() && line[first + remove] == ' ')
				++remove;
			line.erase(first, remove);
		}
		else
			line.insert(first, prefix + " ");
	}
	writeBlock(document, block);
	return true;
}

bool transformLines(EditorDocument &document, const std::function<bool(std::vector<std::string> &)> &change,
	bool wholeDocumentWhenEmpty)
{
	LineBlock block = readBlock(document, wholeDocumentWhenEmpty);
	const auto before = block.lines;
	if (!change(block.lines) || block.lines == before)
		return false;
	writeBlock(document, block);
	return true;
}

bool transformSelection(EditorDocument &document, const std::function<std::string(const std::string &)> &change)
{
	const sptr_t count = document.send(SCI_GETSELECTIONS);
	struct Range
	{
		sptr_t start;
		sptr_t end;
	};
	std::vector<Range> ranges;
	for (sptr_t i = 0; i < count; ++i)
	{
		const sptr_t start = document.send(SCI_GETSELECTIONNSTART, static_cast<uptr_t>(i));
		const sptr_t end = document.send(SCI_GETSELECTIONNEND, static_cast<uptr_t>(i));
		if (end > start)
			ranges.push_back({start, end});
	}
	if (ranges.empty())
		return false;
	// Replace from the end so earlier positions stay valid.
	std::sort(ranges.begin(), ranges.end(), [](const Range &a, const Range &b) { return a.start > b.start; });
	document.send(SCI_BEGINUNDOACTION);
	sptr_t firstStart = 0;
	sptr_t firstEnd = 0;
	for (const auto &range : ranges)
	{
		const std::string replacement = change(textRange(document, range.start, range.end));
		document.send(SCI_SETTARGETRANGE, range.start, range.end);
		document.send(SCI_REPLACETARGET, replacement.size(), reinterpret_cast<sptr_t>(replacement.c_str()));
		firstStart = range.start;
		firstEnd = range.start + static_cast<sptr_t>(replacement.size());
	}
	if (ranges.size() == 1)
		document.send(SCI_SETSEL, firstStart, firstEnd);
	document.send(SCI_ENDUNDOACTION);
	return true;
}

bool setLineComment(EditorDocument &document, const std::string &prefix, bool comment)
{
	if (prefix.empty())
		return false;
	return transformLines(document, [&](std::vector<std::string> &lines) {
		for (auto &line : lines)
		{
			const auto first = line.find_first_not_of(" \t");
			if (first == std::string::npos)
				continue;
			const bool commented = line.compare(first, prefix.size(), prefix) == 0;
			if (comment)
				line.insert(first, prefix + " ");
			else if (commented)
			{
				std::size_t remove = prefix.size();
				if (first + remove < line.size() && line[first + remove] == ' ')
					++remove;
				line.erase(first, remove);
			}
		}
		return true;
	}, false);
}

bool setBlockComment(EditorDocument &document, const std::string &open, const std::string &close, bool comment)
{
	if (open.empty() || close.empty())
		return false;
	sptr_t start = document.send(SCI_GETSELECTIONSTART);
	sptr_t end = document.send(SCI_GETSELECTIONEND);
	if (comment)
	{
		if (start == end)
		{
			// Without a selection the current line is commented.
			const sptr_t line = document.send(SCI_LINEFROMPOSITION, start);
			start = document.send(SCI_POSITIONFROMLINE, line);
			end = document.send(SCI_GETLINEENDPOSITION, line);
		}
		const std::string text = open + textRange(document, start, end) + close;
		document.send(SCI_BEGINUNDOACTION);
		document.send(SCI_SETTARGETRANGE, start, end);
		document.send(SCI_REPLACETARGET, text.size(), reinterpret_cast<sptr_t>(text.c_str()));
		document.send(SCI_SETSEL, start, start + static_cast<sptr_t>(text.size()));
		document.send(SCI_ENDUNDOACTION);
		return true;
	}
	// Uncomment: find the nearest open before the caret and close after it.
	const std::string all = textRange(document, 0, document.send(SCI_GETLENGTH));
	const auto openAt = all.rfind(open, static_cast<std::size_t>(start));
	if (openAt == std::string::npos)
		return false;
	const auto closeAt = all.find(close, std::max<std::size_t>(openAt + open.size(), static_cast<std::size_t>(std::max<sptr_t>(end - static_cast<sptr_t>(close.size()), 0))));
	if (closeAt == std::string::npos || closeAt + close.size() < static_cast<std::size_t>(start))
		return false;
	const std::string inner = all.substr(openAt + open.size(), closeAt - openAt - open.size());
	document.send(SCI_BEGINUNDOACTION);
	document.send(SCI_SETTARGETRANGE, static_cast<sptr_t>(openAt), static_cast<sptr_t>(closeAt + close.size()));
	document.send(SCI_REPLACETARGET, inner.size(), reinterpret_cast<sptr_t>(inner.c_str()));
	document.send(SCI_SETSEL, static_cast<sptr_t>(openAt), static_cast<sptr_t>(openAt + inner.size()));
	document.send(SCI_ENDUNDOACTION);
	return true;
}

std::string lineCommentFor(const std::string &languageId)
{
	static const std::map<std::string, std::string> prefixes = {
		{"cpp", "//"}, {"java", "//"}, {"javascript", "//"}, {"rust", "//"}, {"go", "//"}, {"php", "//"}, {"dart", "//"},
		{"python", "#"}, {"shell", "#"}, {"ruby", "#"}, {"perl", "#"}, {"make", "#"}, {"cmake", "#"}, {"yaml", "#"},
		{"toml", "#"}, {"dockerfile", "#"}, {"powershell", "#"}, {"sql", "--"}, {"lua", "--"}, {"haskell", "--"},
		{"tex", "%"}, {"batch", "REM"}, {"vb", "'"}, {"nsis", ";"}};
	const auto found = prefixes.find(languageId);
	return found == prefixes.end() ? std::string() : found->second;
}
}
