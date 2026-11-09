#ifndef NPP_LINUX_LINE_TOOLS_H
#define NPP_LINUX_LINE_TOOLS_H

#include "EditorDocument.h"

#include <functional>
#include <string>
#include <vector>

namespace NppLinux
{
// These operate on the lines touched by the selection, or on the whole
// document when nothing is selected. Each change is a single undo step.
bool sortLines(EditorDocument &document, bool descending, bool ignoreCase);
std::size_t removeDuplicateLines(EditorDocument &document);
std::size_t trimTrailingWhitespace(EditorDocument &document);
// Adds or removes the prefix on the selected lines (the current line when nothing is selected).
bool toggleLineComment(EditorDocument &document, const std::string &prefix);
// Runs a change over the selected lines (or the document when nothing is
// selected and wholeDocumentWhenEmpty). The callback returns false when it
// changed nothing; the document is then left untouched.
bool transformLines(EditorDocument &document, const std::function<bool(std::vector<std::string> &)> &change,
	bool wholeDocumentWhenEmpty = true);
// Replaces each selection range with change(text) in one undo step. Returns
// false when nothing is selected.
bool transformSelection(EditorDocument &document, const std::function<std::string(const std::string &)> &change);
// Adds (comment=true) or removes the prefix on the selected lines.
bool setLineComment(EditorDocument &document, const std::string &prefix, bool comment);
// Wraps the selection in a block comment, or removes the one around it.
bool setBlockComment(EditorDocument &document, const std::string &open, const std::string &close, bool comment);
// Line comment prefix for a built-in language id, or "" when there is none.
std::string lineCommentFor(const std::string &languageId);
}

#endif
