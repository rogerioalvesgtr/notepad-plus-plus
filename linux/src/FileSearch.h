#ifndef NPP_LINUX_FILE_SEARCH_H
#define NPP_LINUX_FILE_SEARCH_H

#include "EditorDocument.h"
#include "Search.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace NppLinux
{
struct FileSearchOptions
{
	SearchOptions text;
	// Patterns separated by spaces, commas or semicolons. A leading "!" excludes
	// files ("!*.min.js") or folders ("!build/", "!\\build" or "!+\\build").
	std::string filters = "*";
	bool recursive = true;
	bool includeHidden = false;
	std::uintmax_t maxFileSize = 64u * 1024u * 1024u;
	std::size_t maxFiles = 100000;
	std::size_t maxMatches = 50000;
};

struct FileMatch
{
	std::size_t line = 0;   // 1-based
	std::size_t column = 0; // 1-based, in characters
	sptr_t start = 0;
	sptr_t end = 0;
	std::string lineText;
};

struct FileResult
{
	std::filesystem::path path;
	std::string name;                       // label for documents without a path
	const EditorDocument *document = nullptr; // set when the open editor buffer was used
	std::vector<FileMatch> matches;
	std::size_t replaced = 0;
	std::string error;
};

struct FileSearchSummary
{
	std::vector<FileResult> files;
	std::size_t filesScanned = 0;
	std::size_t skippedBinary = 0;
	std::size_t skippedLarge = 0;
	std::size_t unreadable = 0;
	bool truncated = false;
	std::string error;

	std::size_t totalMatches() const;
	std::size_t totalReplaced() const;
	std::size_t filesWithMatches() const;
	std::size_t filesWithErrors() const;
};

class FileFilter
{
public:
	explicit FileFilter(const std::string &filters);
	bool acceptsFile(const std::string &name) const;
	bool acceptsDirectory(const std::string &name) const;

private:
	std::vector<std::string> _include;
	std::vector<std::string> _excludeFiles;
	std::vector<std::string> _excludeDirectories;
};

using OpenDocumentLookup = std::function<EditorDocument *(const std::filesystem::path &)>;

class FileSearcher
{
public:
	FileSearcher();
	~FileSearcher();

	FileSearcher(const FileSearcher &) = delete;
	FileSearcher &operator=(const FileSearcher &) = delete;

	std::vector<std::filesystem::path> collectFiles(const std::filesystem::path &folder,
		const FileSearchOptions &options, FileSearchSummary &summary) const;
	FileSearchSummary find(const std::filesystem::path &folder, const std::string &needle,
		const FileSearchOptions &options, const OpenDocumentLookup &openDocument);
	FileSearchSummary replace(const std::filesystem::path &folder, const std::string &needle,
		const std::string &replacement, const FileSearchOptions &options,
		const OpenDocumentLookup &openDocument);

	// Returns false when the limit stopped the collection before the end of the document.
	static bool collectMatches(EditorDocument &document, const std::string &needle,
		const SearchOptions &options, FileResult &result, std::size_t limit);

private:
	GtkWidget *_widget = nullptr;
	std::unique_ptr<EditorDocument> _scratch;
};
}

#endif
