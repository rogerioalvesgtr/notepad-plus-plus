#include "SessionStore.h"

#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace NppLinux
{
namespace
{
constexpr const char *sessionVersion = "2";

bool readText(const std::filesystem::path &path, std::string &text)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return false;
	std::ostringstream buffer;
	buffer << input.rdbuf();
	text = buffer.str();
	return input.good() || input.eof();
}

bool atomicWrite(const std::filesystem::path &path, const std::string &contents)
{
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if (error)
		return false;
	const std::filesystem::path temporary = path.string() + ".tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			return false;
		output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		output.close();
		if (!output)
		{
			std::filesystem::remove(temporary, error);
			return false;
		}
	}
	std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, error);
	std::filesystem::rename(temporary, path, error);
	if (error)
		std::filesystem::remove(temporary, error);
	return !error;
}

std::string encodeField(std::string_view value)
{
	std::string result;
	static constexpr char digits[] = "0123456789ABCDEF";
	for (const unsigned char byte : value)
	{
		if (byte <= 0x20 || byte == '|' || byte == '%')
		{
			result += '%';
			result += digits[byte >> 4];
			result += digits[byte & 15];
		}
		else
			result += static_cast<char>(byte);
	}
	return result;
}

std::string decodeField(const std::string &value)
{
	std::string result;
	for (std::size_t i = 0; i < value.size(); ++i)
	{
		if (value[i] == '%' && i + 2 < value.size())
		{
			char *end = nullptr;
			const unsigned long byte = std::strtoul(value.substr(i + 1, 2).c_str(), &end, 16);
			if (end && *end == '\0')
			{
				result += static_cast<char>(byte);
				i += 2;
				continue;
			}
		}
		result += value[i];
	}
	return result;
}

std::vector<std::string> splitFields(const std::string &line)
{
	std::vector<std::string> fields;
	std::size_t start = 0;
	while (start <= line.size())
	{
		const auto end = line.find('|', start);
		fields.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return fields;
}

std::string macroPath(const std::filesystem::path &directory)
{
	return (directory / "last-macro.xml").string();
}
}

SessionStore::SessionStore(const std::filesystem::path &configDirectory)
	: _directory(configDirectory)
{
}

std::filesystem::path SessionStore::backupPath(const EditorDocument &document) const
{
	return _directory / "backups" / (document.recoveryId + ".txt");
}

bool SessionStore::backup(const EditorDocument &document)
{
	if (document.recoveryId.empty())
		return false;
	return atomicWrite(backupPath(document), document.text());
}

void SessionStore::discardBackup(const EditorDocument &document)
{
	std::error_code error;
	if (!document.recoveryId.empty())
		std::filesystem::remove(backupPath(document), error);
}

std::vector<SessionEntry> SessionStore::load() const
{
	std::string contents;
	if (!readText(_directory / "session.txt", contents))
		return {};
	std::vector<SessionEntry> entries;
	std::istringstream input(contents);
	std::string line;
	while (std::getline(input, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const auto fields = splitFields(line);
		if (fields.empty())
			continue;
		if (fields[0] == sessionVersion)
			continue;
		if (fields[0] == "1" && fields.size() >= 2)
		{
			SessionEntry entry;
			entry.path = decodeField(fields[1]);
			entries.push_back(std::move(entry));
			continue;
		}
		if (fields[0] != "file" && fields[0] != "untitled")
			continue;
		SessionEntry entry;
		const bool untitled = fields[0] == "untitled";
		if (fields.size() > 1) entry.name = decodeField(fields[1]);
		if (fields.size() > 2) entry.path = untitled ? std::string() : decodeField(fields[2]);
		if (fields.size() > 3) entry.backup = decodeField(fields[3]);
		if (fields.size() > 4) entry.language = decodeField(fields[4]);
		if (fields.size() > 5) entry.encoding = decodeField(fields[5]);
		if (fields.size() > 6 && !fields[6].empty()) entry.eol = std::clamp(std::atoi(fields[6].c_str()), 0, 2);
		if (fields.size() > 7) entry.modified = fields[7] == "1";
		entries.push_back(std::move(entry));
	}
	return entries;
}

bool SessionStore::save(const std::vector<std::unique_ptr<EditorDocument>> &documents) const
{
	std::ostringstream output;
	output << sessionVersion << '\n';
	for (const auto &document : documents)
	{
		if (!document)
			continue;
		const bool untitled = document->path.empty();
		if (untitled && document->recoveryId.empty())
			continue;
		output << (untitled ? "untitled" : "file") << '|'
			<< encodeField(document->displayName) << '|'
			<< encodeField(document->path.string()) << '|'
			<< encodeField(document->recoveryId) << '|'
			<< encodeField(document->language->id) << '|'
			<< encodeField(document->encoding) << '|'
			<< static_cast<int>(document->send(SCI_GETEOLMODE)) << '|'
			<< (document->send(SCI_GETMODIFY) ? '1' : '0') << '\n';
	}
	return atomicWrite(_directory / "session.txt", output.str());
}

bool SessionStore::isAllowed(const MacroCommand &command)
{
	if (command.text.size() > 16 * 1024 * 1024)
		return false;
	const bool textual = command.message == SCI_REPLACESEL || command.message == SCI_ADDTEXT
		|| command.message == SCI_INSERTTEXT || command.message == SCI_APPENDTEXT
		|| command.message == SCI_SEARCHNEXT || command.message == SCI_SEARCHPREV;
	if (textual != command.hasText || !command.hasText && command.lParam != 0)
		return false;
	switch (command.message)
	{
		case SCI_REPLACESEL:
			return command.wParam == 0;
		case SCI_ADDTEXT: case SCI_APPENDTEXT:
			return command.wParam == command.text.size();
		case SCI_INSERTTEXT:
			return command.wParam == static_cast<uptr_t>(-1)
				|| command.wParam <= static_cast<uptr_t>(std::numeric_limits<sptr_t>::max());
		case SCI_CUT: case SCI_COPY: case SCI_PASTE: case SCI_CLEAR: case SCI_CLEARALL:
		case SCI_SELECTALL: case SCI_DELETEBACK: case SCI_TAB: case SCI_BACKTAB:
		case SCI_DELWORDLEFT: case SCI_DELWORDRIGHT: case SCI_DELWORDRIGHTEND:
		case SCI_LINECOPY: case SCI_LINECUT: case SCI_LINEDELETE:
		case SCI_LINETRANSPOSE: case SCI_LINEDUPLICATE: case SCI_LOWERCASE: case SCI_UPPERCASE:
		case SCI_DELETEBACKNOTLINE: case SCI_SELECTIONDUPLICATE:
			return command.wParam == 0 && command.lParam == 0;
		case SCI_GOTOLINE: case SCI_GOTOPOS:
		case SCI_SEARCHANCHOR:
			return true;
		case SCI_SEARCHNEXT: case SCI_SEARCHPREV:
			return command.wParam <= (SCFIND_REGEXP | SCFIND_POSIX | SCFIND_MATCHCASE | SCFIND_WHOLEWORD | SCFIND_WORDSTART)
				&& command.text.size() <= 1024 * 1024;
		default:
			if (command.wParam != 0)
				return false;
			switch (command.message)
			{
				case SCI_LINEDOWN: case SCI_LINEDOWNEXTEND: case SCI_PARADOWN: case SCI_PARADOWNEXTEND:
				case SCI_LINEUP: case SCI_LINEUPEXTEND: case SCI_PARAUP: case SCI_PARAUPEXTEND:
				case SCI_CHARLEFT: case SCI_CHARLEFTEXTEND: case SCI_CHARRIGHT: case SCI_CHARRIGHTEXTEND:
				case SCI_WORDLEFT: case SCI_WORDLEFTEXTEND: case SCI_WORDRIGHT: case SCI_WORDRIGHTEXTEND:
				case SCI_WORDPARTLEFT: case SCI_WORDPARTLEFTEXTEND: case SCI_WORDPARTRIGHT: case SCI_WORDPARTRIGHTEXTEND:
				case SCI_WORDLEFTEND: case SCI_WORDLEFTENDEXTEND: case SCI_WORDRIGHTEND: case SCI_WORDRIGHTENDEXTEND:
				case SCI_HOME: case SCI_HOMEEXTEND: case SCI_LINEEND: case SCI_LINEENDEXTEND:
				case SCI_HOMEWRAP: case SCI_HOMEWRAPEXTEND: case SCI_LINEENDWRAP: case SCI_LINEENDWRAPEXTEND:
				case SCI_DOCUMENTSTART: case SCI_DOCUMENTSTARTEXTEND: case SCI_DOCUMENTEND: case SCI_DOCUMENTENDEXTEND:
				case SCI_PAGEUP: case SCI_PAGEUPEXTEND: case SCI_PAGEDOWN: case SCI_PAGEDOWNEXTEND:
					return true;
				case SCI_EDITTOGGLEOVERTYPE: case SCI_CANCEL: case SCI_VCHOME: case SCI_VCHOMEEXTEND:
				case SCI_VCHOMEWRAP: case SCI_VCHOMEWRAPEXTEND: case SCI_VCHOMEDISPLAY: case SCI_VCHOMEDISPLAYEXTEND:
				case SCI_HOMEDISPLAY: case SCI_HOMEDISPLAYEXTEND: case SCI_LINEENDDISPLAY: case SCI_LINEENDDISPLAYEXTEND:
				case SCI_LINEDOWNRECTEXTEND: case SCI_LINEUPRECTEXTEND: case SCI_CHARLEFTRECTEXTEND:
				case SCI_CHARRIGHTRECTEXTEND: case SCI_HOMERECTEXTEND: case SCI_VCHOMERECTEXTEND:
				case SCI_LINEENDRECTEXTEND: case SCI_PAGEUPRECTEXTEND: case SCI_PAGEDOWNRECTEXTEND:
				case SCI_MOVESELECTEDLINESUP: case SCI_MOVESELECTEDLINESDOWN:
					return command.lParam == 0;
				default:
					return false;
			}
	}
}

std::vector<MacroCommand> SessionStore::loadMacro() const
{
	return loadMacroFile(macroPath(_directory));
}

bool SessionStore::saveMacro(const std::vector<MacroCommand> &commands) const
{
	return saveMacroFile(macroPath(_directory), commands);
}

std::filesystem::path SessionStore::macroDirectory() const
{
	return _directory / "macros";
}

bool SessionStore::saveSessionFile(const std::filesystem::path &file, const std::vector<std::string> &paths)
{
	std::ostringstream output;
	output << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<NotepadPlus>\n    <Session activeView=\"0\">\n        <mainView activeIndex=\"0\">\n";
	for (const auto &path : paths)
		output << "            <File filename=\"" << escapeXml(path) << "\" />\n";
	output << "        </mainView>\n        <subView activeIndex=\"0\" />\n    </Session>\n</NotepadPlus>\n";
	return atomicWrite(file, output.str());
}

std::vector<std::string> SessionStore::loadSessionFile(const std::filesystem::path &file)
{
	std::string xml;
	std::vector<std::string> paths;
	if (!readText(file, xml) || xml.find("<Session") == std::string::npos)
		return paths;
	std::size_t position = 0;
	while ((position = xml.find("<File ", position)) != std::string::npos)
	{
		const auto end = xml.find('>', position);
		if (end == std::string::npos)
			break;
		const std::string tag = xml.substr(position, end - position);
		const auto name = tag.find("filename=\"");
		if (name != std::string::npos)
		{
			const auto close = tag.find('"', name + 10);
			if (close != std::string::npos)
				paths.push_back(unescapeXml(tag.substr(name + 10, close - name - 10)));
		}
		position = end;
	}
	return paths;
}

std::vector<MacroCommand> SessionStore::loadMacroFile(const std::filesystem::path &file)
{
	std::string xml;
	if (!readText(file, xml))
		return {};
	std::vector<MacroCommand> commands;
	if (xml.find("<ScintillaMacro version=\"1\">") == std::string::npos || xml.find("</ScintillaMacro>") == std::string::npos)
		return {};
	std::size_t start = 0;
	while ((start = xml.find("<command ", start)) != std::string::npos)
	{
		const auto end = xml.find("/>", start);
		if (end == std::string::npos || commands.size() >= 10000)
			return {};
		const std::string tag = xml.substr(start, end - start);
		auto number = [&](const std::string &name) -> unsigned long long {
			const auto position = tag.find(name + "=\"");
			if (position == std::string::npos)
				return 0;
			const auto digits = tag.substr(position + name.size() + 2);
			if (digits.empty() || (!std::isdigit(static_cast<unsigned char>(digits.front())) && digits.front() != '-'))
				return 0;
			try
			{
				return std::stoull(digits);
			}
			catch (...)
			{
				return 0;
			}
		};
		MacroCommand command;
		command.message = static_cast<unsigned int>(number("message"));
		command.wParam = static_cast<uptr_t>(number("wparam"));
		command.lParam = static_cast<sptr_t>(number("lparam"));
		const auto textPosition = tag.find("text=\"");
		if (textPosition != std::string::npos)
		{
			const auto textEnd = tag.find('"', textPosition + 6);
			if (textEnd == std::string::npos)
				return {};
			command.text = unescapeXml(tag.substr(textPosition + 6, textEnd - textPosition - 6));
			command.hasText = true;
		}
		if (commands.size() >= 10000 || !isAllowed(command))
			return {};
		commands.push_back(std::move(command));
		start = end + 2;
	}
	return commands;
}

bool SessionStore::saveMacroFile(const std::filesystem::path &file, const std::vector<MacroCommand> &commands)
{
	if (commands.size() > 10000)
		return false;
	if (std::any_of(commands.begin(), commands.end(), [](const MacroCommand &command) { return !isAllowed(command); }))
		return false;
	std::ostringstream output;
	output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ScintillaMacro version=\"1\">\n";
	for (const auto &command : commands)
	{
		output << "\t<command message=\"" << command.message << "\" wparam=\"" << command.wParam
			<< "\" lparam=\"" << command.lParam << "\"";
		if (command.hasText)
			output << " text=\"" << escapeXml(command.text) << "\"";
		output << "/>\n";
	}
	output << "</ScintillaMacro>\n";
	std::error_code error;
	std::filesystem::create_directories(file.parent_path(), error);
	return atomicWrite(file, output.str());
}
}
