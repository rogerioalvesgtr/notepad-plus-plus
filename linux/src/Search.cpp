#include "Search.h"

#include <algorithm>

#include "Scintilla.h"

namespace NppLinux
{
namespace
{
int flags(const SearchOptions &options)
{
	int result = options.regularExpression ? SCFIND_REGEXP : SCFIND_NONE;
	if (options.matchCase)
		result |= SCFIND_MATCHCASE;
	if (options.wholeWord)
		result |= SCFIND_WHOLEWORD;
	return result;
}

SearchResult searchRange(EditorDocument &document, const std::string &needle,
	const SearchOptions &options, sptr_t start, sptr_t end, bool backwards = false)
{
	if (needle.empty() || (backwards ? start < end : end <= start))
		return {};
	const int searchFlags = flags(options);
	document.send(SCI_SETSEARCHFLAGS, searchFlags);
	document.send(SCI_SETTARGETSTART, start);
	document.send(SCI_SETTARGETEND, end);
	const sptr_t position = document.send(SCI_SEARCHINTARGET, static_cast<uptr_t>(needle.size()), reinterpret_cast<sptr_t>(needle.c_str()));
	if (position < 0)
		return {};
	document.send(SCI_SETSEL, document.send(SCI_GETTARGETSTART), document.send(SCI_GETTARGETEND));
	document.send(SCI_SCROLLCARET);
	return {true, false, 1};
}

bool currentSelectionMatches(EditorDocument &document, const std::string &needle, const SearchOptions &options)
{
	if (needle.empty())
		return false;
	const sptr_t selectionStart = std::min(document.send(SCI_GETCURRENTPOS), document.send(SCI_GETANCHOR));
	const sptr_t selectionEnd = std::max(document.send(SCI_GETCURRENTPOS), document.send(SCI_GETANCHOR));
	const sptr_t selectionLength = selectionEnd - selectionStart;
	if (selectionLength == 0)
		return false;
	document.send(SCI_SETSEARCHFLAGS, flags(options));
	document.send(SCI_SETTARGETSTART, selectionStart);
	document.send(SCI_SETTARGETEND, selectionStart + selectionLength);
	const sptr_t result = document.send(SCI_SEARCHINTARGET, static_cast<uptr_t>(needle.size()), reinterpret_cast<sptr_t>(needle.c_str()));
	return result == selectionStart
		&& document.send(SCI_GETTARGETSTART) == selectionStart
		&& document.send(SCI_GETTARGETEND) == selectionStart + selectionLength;
}
}

SearchResult find(EditorDocument &document, const std::string &needle,
	const SearchOptions &options, bool backwards)
{
	if (needle.empty())
		return {};
	const sptr_t length = document.send(SCI_GETLENGTH);
	const sptr_t caret = document.send(SCI_GETCURRENTPOS);
	const sptr_t anchor = document.send(SCI_GETANCHOR);
	SearchResult result;
	if (backwards)
	{
		result = searchRange(document, needle, options, std::min(caret, anchor), 0, true);
		if (!result.found && options.wrapAround)
		{
			result = searchRange(document, needle, options, length, std::min(caret, anchor), true);
			result.wrapped = result.found;
		}
	}
	else
	{
		result = searchRange(document, needle, options, std::max(caret, anchor), length);
		if (!result.found && options.wrapAround)
		{
			result = searchRange(document, needle, options, 0, std::max(caret, anchor));
			result.wrapped = result.found;
		}
	}
	return result;
}

std::size_t replaceCurrent(EditorDocument &document, const std::string &needle,
	const std::string &replacement, const SearchOptions &options)
{
	if (!currentSelectionMatches(document, needle, options))
		return 0;
	document.send(SCI_TARGETFROMSELECTION);
	document.sendString(options.regularExpression ? SCI_REPLACETARGETRE : SCI_REPLACETARGET,
		static_cast<uptr_t>(replacement.size()), replacement);
	return 1;
}

std::size_t replaceAll(EditorDocument &document, const std::string &needle,
	const std::string &replacement, const SearchOptions &options)
{
	if (needle.empty())
		return 0;
	std::size_t count = 0;
	const sptr_t caret = document.send(SCI_GETCURRENTPOS);
	const sptr_t anchor = document.send(SCI_GETANCHOR);
	document.send(SCI_BEGINUNDOACTION);
	sptr_t start = 0;
	while (start <= document.send(SCI_GETLENGTH))
	{
		document.send(SCI_SETSEARCHFLAGS, flags(options));
		document.send(SCI_SETTARGETSTART, start);
		document.send(SCI_SETTARGETEND, document.send(SCI_GETLENGTH));
		const sptr_t result = document.send(SCI_SEARCHINTARGET, static_cast<uptr_t>(needle.size()), reinterpret_cast<sptr_t>(needle.c_str()));
		if (result < 0)
			break;
		const sptr_t matchStart = document.send(SCI_GETTARGETSTART);
		const sptr_t matchEnd = document.send(SCI_GETTARGETEND);
		const sptr_t matchLength = matchEnd - matchStart;
		document.sendString(options.regularExpression ? SCI_REPLACETARGETRE : SCI_REPLACETARGET,
			static_cast<uptr_t>(replacement.size()), replacement);
		++count;
		start = document.send(SCI_GETTARGETEND);
		if (matchLength == 0 && replacement.empty())
			++start;
	}
	document.send(SCI_ENDUNDOACTION);
	document.send(SCI_SETSEL, anchor, caret);
	document.send(SCI_SCROLLCARET);
	return count;
}

std::size_t countMatches(EditorDocument &document, const std::string &needle,
	const SearchOptions &options)
{
	if (needle.empty())
		return 0;
	std::size_t count = 0;
	sptr_t start = 0;
	const sptr_t length = document.send(SCI_GETLENGTH);
	while (start <= length)
	{
		document.send(SCI_SETSEARCHFLAGS, flags(options));
		document.send(SCI_SETTARGETSTART, start);
		document.send(SCI_SETTARGETEND, length);
		if (document.send(SCI_SEARCHINTARGET, static_cast<uptr_t>(needle.size()), reinterpret_cast<sptr_t>(needle.c_str())) < 0)
			break;
		const sptr_t matchStart = document.send(SCI_GETTARGETSTART);
		const sptr_t matchEnd = document.send(SCI_GETTARGETEND);
		++count;
		start = matchEnd;
		if (matchEnd == matchStart)
			++start;
	}
	return count;
}

bool goToLine(EditorDocument &document, long line)
{
	const long lines = static_cast<long>(document.send(SCI_GETLINECOUNT));
	if (line < 1 || line > lines)
		return false;
	document.send(SCI_GOTOLINE, static_cast<uptr_t>(line - 1));
	document.send(SCI_SCROLLCARET);
	return true;
}

int searchFlags(const SearchOptions &options)
{
	return flags(options);
}

bool validateRegularExpression(const std::string &expression)
{
	std::string escaped = expression;
	std::size_t position = 0;
	while ((position = escaped.find("\\(", position)) != std::string::npos)
	{
		escaped.erase(position, 1);
		position += 1;
	}
	position = 0;
	while ((position = escaped.find("\\)", position)) != std::string::npos)
	{
		escaped.erase(position, 1);
		position += 1;
	}
	GRegex *regex = g_regex_new(escaped.c_str(), static_cast<GRegexCompileFlags>(0), static_cast<GRegexMatchFlags>(0), nullptr);
	if (!regex)
		return false;
	g_regex_unref(regex);
	return true;
}
}
