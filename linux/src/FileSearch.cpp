#include "FileSearch.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>

#include "Scintilla.h"
#include "ScintillaWidget.h"

namespace NppLinux
{
namespace
{
constexpr std::size_t maxLineText = 400;

std::string fold(const std::string &text)
{
	if (!g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr))
	{
		std::string lower = text;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(g_ascii_tolower(c)); });
		return lower;
	}
	gchar *folded = g_utf8_casefold(text.c_str(), static_cast<gssize>(text.size()));
	std::string result = folded ? folded : text;
	g_free(folded);
	return result;
}

bool matchesAny(const std::vector<std::string> &patterns, const std::string &name)
{
	return std::any_of(patterns.begin(), patterns.end(), [&](const std::string &pattern) {
		return g_pattern_match_simple(pattern.c_str(), name.c_str()) != FALSE;
	});
}

bool isHidden(const std::string &name)
{
	return !name.empty() && name.front() == '.';
}

enum class ReadStatus
{
	Ok,
	Binary,
	Large,
	Unreadable
};

struct LoadedFile
{
	std::string text;
	bool bom = false;
	bool utf8 = true;
	std::filesystem::file_time_type modified;
};

ReadStatus readTextFile(const std::filesystem::path &path, std::uintmax_t maxSize, LoadedFile &file)
{
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error)
		return ReadStatus::Unreadable;
	if (size > maxSize)
		return ReadStatus::Large;
	file.modified = std::filesystem::last_write_time(path, error);
	if (error)
		return ReadStatus::Unreadable;
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return ReadStatus::Unreadable;
	std::ostringstream buffer;
	buffer << input.rdbuf();
	if (input.bad())
		return ReadStatus::Unreadable;
	const std::string bytes = buffer.str();
	// UTF-16/32 files and binaries contain NUL bytes; they are not edited as text.
	if (bytes.find('\0') != std::string::npos)
		return ReadStatus::Binary;
	file.bom = bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0;
	std::string encoding;
	file.text = normaliseUtf8(bytes, encoding);
	file.utf8 = encoding == "UTF-8";
	return ReadStatus::Ok;
}

void loadText(EditorDocument &document, const std::string &text)
{
	document.send(SCI_CLEARALL);
	document.send(SCI_ADDTEXT, static_cast<uptr_t>(text.size()), reinterpret_cast<sptr_t>(text.data()));
	document.send(SCI_SETSEL, 0, 0);
}

std::string lineAt(EditorDocument &document, sptr_t line)
{
	const sptr_t length = document.send(SCI_LINELENGTH, static_cast<uptr_t>(line));
	std::string text(static_cast<std::size_t>(std::max<sptr_t>(length, 0)) + 1, '\0');
	document.send(SCI_GETLINE, static_cast<uptr_t>(line), reinterpret_cast<sptr_t>(text.data()));
	text.resize(static_cast<std::size_t>(std::max<sptr_t>(length, 0)));
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	std::string expanded;
	expanded.reserve(text.size());
	for (const char c : text)
	{
		if (c == '\t')
			expanded += "    ";
		else
			expanded += c;
	}
	const auto first = expanded.find_first_not_of(' ');
	expanded.erase(0, first == std::string::npos ? expanded.size() : first);
	if (expanded.size() > maxLineText)
	{
		const char *begin = expanded.c_str();
		const char *cut = g_utf8_find_prev_char(begin, begin + maxLineText + 1);
		expanded.resize(cut ? static_cast<std::size_t>(cut - begin) : maxLineText);
		expanded += "…";
	}
	return expanded;
}

bool writeAtomically(const std::filesystem::path &path, const std::string &bytes,
	std::filesystem::file_time_type expected, std::string &error)
{
	std::error_code code;
	// Resolve symbolic links so the link itself is kept and its target is updated.
	const std::filesystem::path target = std::filesystem::canonical(path, code);
	if (code)
	{
		error = "não foi possível localizar o arquivo: " + code.message();
		return false;
	}
	if (::access(target.c_str(), W_OK) != 0)
	{
		error = "sem permissão de escrita; arquivo não alterado";
		return false;
	}
	if (std::filesystem::last_write_time(target, code) != expected || code)
	{
		error = "o arquivo mudou durante a operação; arquivo não alterado";
		return false;
	}
	struct stat information {};
	if (::stat(target.c_str(), &information) != 0)
	{
		error = std::strerror(errno);
		return false;
	}
	std::string pattern = (target.parent_path() / ("." + target.filename().string() + ".npp-linux-XXXXXX")).string();
	std::vector<char> temporary(pattern.begin(), pattern.end());
	temporary.push_back('\0');
	const int descriptor = ::mkstemp(temporary.data());
	if (descriptor < 0)
	{
		error = "não foi possível criar arquivo temporário: " + std::string(std::strerror(errno));
		return false;
	}
	std::size_t written = 0;
	bool ok = true;
	while (written < bytes.size())
	{
		const ssize_t count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
		if (count < 0)
		{
			if (errno == EINTR)
				continue;
			ok = false;
			break;
		}
		written += static_cast<std::size_t>(count);
	}
	if (ok)
		ok = ::fchmod(descriptor, information.st_mode & 07777) == 0 && ::fsync(descriptor) == 0;
	const int savedErrno = errno;
	if (::close(descriptor) != 0)
		ok = false;
	if (ok && ::rename(temporary.data(), target.c_str()) == 0)
		return true;
	error = "não foi possível gravar: " + std::string(std::strerror(ok ? errno : savedErrno));
	::unlink(temporary.data());
	return false;
}
}

std::size_t FileSearchSummary::totalMatches() const
{
	std::size_t total = 0;
	for (const auto &file : files)
		total += file.matches.size();
	return total;
}

std::size_t FileSearchSummary::totalReplaced() const
{
	std::size_t total = 0;
	for (const auto &file : files)
		total += file.replaced;
	return total;
}

std::size_t FileSearchSummary::filesWithMatches() const
{
	return static_cast<std::size_t>(std::count_if(files.begin(), files.end(), [](const FileResult &file) {
		return !file.matches.empty() || file.replaced > 0;
	}));
}

std::size_t FileSearchSummary::filesWithErrors() const
{
	return static_cast<std::size_t>(std::count_if(files.begin(), files.end(), [](const FileResult &file) {
		return !file.error.empty();
	}));
}

FileFilter::FileFilter(const std::string &filters)
{
	std::string token;
	auto flush = [&]() {
		if (token.empty())
			return;
		if (token.front() == '!')
		{
			std::string pattern = token.substr(1);
			if (!pattern.empty() && pattern.front() == '+')
				pattern.erase(0, 1);
			bool directory = false;
			while (!pattern.empty() && (pattern.front() == '/' || pattern.front() == '\\'))
			{
				pattern.erase(0, 1);
				directory = true;
			}
			while (!pattern.empty() && (pattern.back() == '/' || pattern.back() == '\\'))
			{
				pattern.pop_back();
				directory = true;
			}
			if (!pattern.empty())
				(directory ? _excludeDirectories : _excludeFiles).push_back(fold(pattern));
		}
		else
			_include.push_back(token == "*.*" ? "*" : fold(token));
		token.clear();
	};
	for (const char c : filters)
	{
		if (c == ';' || c == ',' || g_ascii_isspace(c))
			flush();
		else
			token += c;
	}
	flush();
	if (_include.empty())
		_include.push_back("*");
}

bool FileFilter::acceptsFile(const std::string &name) const
{
	const std::string folded = fold(name);
	return matchesAny(_include, folded) && !matchesAny(_excludeFiles, folded);
}

bool FileFilter::acceptsDirectory(const std::string &name) const
{
	return !matchesAny(_excludeDirectories, fold(name));
}

FileSearcher::FileSearcher()
{
	_widget = scintilla_new();
	g_object_ref_sink(_widget);
	_scratch = std::make_unique<EditorDocument>(_widget);
	_scratch->send(SCI_SETUNDOCOLLECTION, 0);
}

FileSearcher::~FileSearcher()
{
	_scratch.reset();
	gtk_widget_destroy(_widget);
	g_object_unref(_widget);
}

std::vector<std::filesystem::path> FileSearcher::collectFiles(const std::filesystem::path &folder,
	const FileSearchOptions &options, FileSearchSummary &summary) const
{
	std::vector<std::filesystem::path> files;
	std::error_code error;
	const std::filesystem::path root = std::filesystem::absolute(folder, error).lexically_normal();
	if (error || !std::filesystem::is_directory(root, error))
	{
		summary.error = "A pasta não existe ou não pode ser aberta: " + folder.string();
		return files;
	}
	const FileFilter filter(options.filters);
	auto consider = [&](const std::filesystem::directory_entry &entry, bool &skipDirectory) {
		skipDirectory = false;
		std::error_code entryError;
		const std::string name = entry.path().filename().string();
		if (entry.is_directory(entryError))
		{
			skipDirectory = (!options.includeHidden && isHidden(name)) || !filter.acceptsDirectory(name)
				|| entry.is_symlink(entryError);
			return;
		}
		if (!entry.is_regular_file(entryError))
			return;
		if (!options.includeHidden && isHidden(name))
			return;
		if (filter.acceptsFile(name))
			files.push_back(entry.path());
	};
	const auto directoryOptions = std::filesystem::directory_options::skip_permission_denied;
	if (options.recursive)
	{
		std::filesystem::recursive_directory_iterator iterator(root, directoryOptions, error);
		for (; !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
		{
			bool skipDirectory = false;
			consider(*iterator, skipDirectory);
			if (skipDirectory)
				iterator.disable_recursion_pending();
			if (files.size() >= options.maxFiles)
			{
				summary.truncated = true;
				break;
			}
		}
	}
	else
	{
		std::filesystem::directory_iterator iterator(root, directoryOptions, error);
		for (; !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error))
		{
			bool skipDirectory = false;
			consider(*iterator, skipDirectory);
			if (files.size() >= options.maxFiles)
			{
				summary.truncated = true;
				break;
			}
		}
	}
	if (error)
		++summary.unreadable;
	std::sort(files.begin(), files.end());
	return files;
}

bool FileSearcher::collectMatches(EditorDocument &document, const std::string &needle,
	const SearchOptions &options, FileResult &result, std::size_t limit)
{
	if (needle.empty())
		return true;
	const sptr_t length = document.send(SCI_GETLENGTH);
	sptr_t start = 0;
	sptr_t cachedLine = -1;
	std::string cachedText;
	while (start <= length)
	{
		if (result.matches.size() >= limit)
			return false;
		document.send(SCI_SETSEARCHFLAGS, searchFlags(options));
		document.send(SCI_SETTARGETSTART, start);
		document.send(SCI_SETTARGETEND, length);
		if (document.send(SCI_SEARCHINTARGET, static_cast<uptr_t>(needle.size()), reinterpret_cast<sptr_t>(needle.c_str())) < 0)
			break;
		const sptr_t matchStart = document.send(SCI_GETTARGETSTART);
		const sptr_t matchEnd = document.send(SCI_GETTARGETEND);
		const sptr_t line = document.send(SCI_LINEFROMPOSITION, matchStart);
		if (line != cachedLine)
		{
			cachedLine = line;
			cachedText = lineAt(document, line);
		}
		FileMatch match;
		match.line = static_cast<std::size_t>(line) + 1;
		match.column = static_cast<std::size_t>(document.send(SCI_COUNTCHARACTERS,
			static_cast<uptr_t>(document.send(SCI_POSITIONFROMLINE, static_cast<uptr_t>(line))), matchStart)) + 1;
		match.start = matchStart;
		match.end = matchEnd;
		match.lineText = cachedText;
		result.matches.push_back(std::move(match));
		start = matchEnd > matchStart ? matchEnd : document.send(SCI_POSITIONAFTER, matchEnd);
		if (start <= matchStart)
			break;
	}
	return true;
}

FileSearchSummary FileSearcher::find(const std::filesystem::path &folder, const std::string &needle,
	const FileSearchOptions &options, const OpenDocumentLookup &openDocument)
{
	FileSearchSummary summary;
	const auto files = collectFiles(folder, options, summary);
	if (!summary.error.empty() || needle.empty())
		return summary;
	std::size_t total = 0;
	for (const auto &path : files)
	{
		if (total >= options.maxMatches)
		{
			summary.truncated = true;
			break;
		}
		FileResult result;
		result.path = path;
		EditorDocument *document = openDocument ? openDocument(path) : nullptr;
		if (document)
		{
			result.document = document;
			if (!collectMatches(*document, needle, options.text, result, options.maxMatches - total))
				summary.truncated = true;
		}
		else
		{
			LoadedFile file;
			switch (readTextFile(path, options.maxFileSize, file))
			{
				case ReadStatus::Binary:
					++summary.skippedBinary;
					continue;
				case ReadStatus::Large:
					++summary.skippedLarge;
					continue;
				case ReadStatus::Unreadable:
					++summary.unreadable;
					continue;
				case ReadStatus::Ok:
					break;
			}
			loadText(*_scratch, file.text);
			if (!collectMatches(*_scratch, needle, options.text, result, options.maxMatches - total))
				summary.truncated = true;
		}
		++summary.filesScanned;
		total += result.matches.size();
		if (!result.matches.empty())
			summary.files.push_back(std::move(result));
	}
	_scratch->send(SCI_CLEARALL);
	return summary;
}

FileSearchSummary FileSearcher::replace(const std::filesystem::path &folder, const std::string &needle,
	const std::string &replacement, const FileSearchOptions &options,
	const OpenDocumentLookup &openDocument)
{
	FileSearchSummary summary;
	const auto files = collectFiles(folder, options, summary);
	if (!summary.error.empty() || needle.empty())
		return summary;
	for (const auto &path : files)
	{
		FileResult result;
		result.path = path;
		if (EditorDocument *document = openDocument ? openDocument(path) : nullptr)
		{
			// Open files are changed only in the editor so the change can be undone and reviewed.
			result.document = document;
			result.replaced = replaceAll(*document, needle, replacement, options.text);
			++summary.filesScanned;
			if (result.replaced)
				summary.files.push_back(std::move(result));
			continue;
		}
		LoadedFile file;
		switch (readTextFile(path, options.maxFileSize, file))
		{
			case ReadStatus::Binary:
				++summary.skippedBinary;
				continue;
			case ReadStatus::Large:
				++summary.skippedLarge;
				continue;
			case ReadStatus::Unreadable:
				++summary.unreadable;
				result.error = "não foi possível ler o arquivo";
				summary.files.push_back(std::move(result));
				continue;
			case ReadStatus::Ok:
				break;
		}
		++summary.filesScanned;
		loadText(*_scratch, file.text);
		const std::size_t count = replaceAll(*_scratch, needle, replacement, options.text);
		if (count == 0)
			continue;
		if (!file.utf8)
		{
			result.error = "codificação diferente de UTF-8; arquivo não alterado";
			summary.files.push_back(std::move(result));
			continue;
		}
		const std::string bytes = (file.bom ? std::string("\xEF\xBB\xBF") : std::string()) + _scratch->text();
		std::string error;
		if (writeAtomically(path, bytes, file.modified, error))
			result.replaced = count;
		else
			result.error = error;
		summary.files.push_back(std::move(result));
	}
	_scratch->send(SCI_CLEARALL);
	return summary;
}
}
