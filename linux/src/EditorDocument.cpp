#include "EditorDocument.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <glib.h>

#include "Scintilla.h"
#include "ScintillaWidget.h"
#include "TextTools.h"

namespace NppLinux
{
namespace
{
std::string ioError(const std::filesystem::path &path)
{
	return path.string() + ": " + std::strerror(errno);
}
}

EditorDocument::EditorDocument(GtkWidget *widget)
	: displayName("Sem título"), _widget(widget)
{
	recoveryId = std::to_string(static_cast<long long>(g_get_real_time())) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
	send(SCI_SETCODEPAGE, SC_CP_UTF8);
	send(SCI_SETBUFFEREDDRAW, 1);
	send(SCI_SETENDATLASTLINE, 1);
	send(SCI_SETSCROLLWIDTHTRACKING, 1);
	send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
	send(SCI_SETEOLMODE, SC_EOL_LF);
}

EditorDocument::~EditorDocument() = default;

GtkWidget *EditorDocument::widget() const noexcept
{
	return _widget;
}

sptr_t EditorDocument::send(unsigned int message, uptr_t wParam, sptr_t lParam) const
{
	return scintilla_send_message(SCINTILLA(_widget), message, wParam, lParam);
}

void EditorDocument::sendString(unsigned int message, uptr_t wParam, const std::string &value) const
{
	send(message, wParam, reinterpret_cast<sptr_t>(value.c_str()));
}

std::string EditorDocument::text() const
{
	const auto length = static_cast<std::size_t>(send(SCI_GETTEXTLENGTH));
	std::string result(length + 1, '\0');
	send(SCI_GETTEXT, static_cast<uptr_t>(length + 1), reinterpret_cast<sptr_t>(result.data()));
	result.resize(length);
	return result;
}

void EditorDocument::setText(const std::string &text)
{
	// Change history starts again from the loaded text, so it is not shown as edited.
	const sptr_t history = send(SCI_GETCHANGEHISTORY);
	send(SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED);
	sendString(SCI_SETTEXT, 0, text);
	send(SCI_EMPTYUNDOBUFFER);
	send(SCI_SETSAVEPOINT);
	send(SCI_SETCHANGEHISTORY, static_cast<uptr_t>(history));
	dirty = false;
}

bool EditorDocument::load(const std::filesystem::path &filePath, std::string &error, const std::string &forcedEncoding)
{
	std::ifstream input(filePath, std::ios::binary);
	if (!input)
	{
		error = ioError(filePath);
		return false;
	}
	std::ostringstream buffer;
	buffer << input.rdbuf();
	if (!input.good() && !input.eof())
	{
		error = "Não foi possível ler o arquivo: " + filePath.string();
		return false;
	}
	const std::string bytes = buffer.str();
	std::string decoded;
	std::string detected;
	if (!decodeBytes(bytes, forcedEncoding, detected, decoded, error))
	{
		error = filePath.string() + ": não foi possível ler como " + encodingLabel(forcedEncoding) + ". " + error;
		return false;
	}
	path = std::filesystem::absolute(filePath).lexically_normal();
	displayName = path.filename().string();
	encoding = detected;
	eolMode = detectEol(decoded);
	language = &languageForPath(path);
	send(SCI_SETEOLMODE, eolMode == EolMode::Windows ? SC_EOL_CRLF : eolMode == EolMode::MacClassic ? SC_EOL_CR : SC_EOL_LF);
	const bool wasReadOnly = send(SCI_GETREADONLY) != 0;
	send(SCI_SETREADONLY, 0);
	setText(decoded);
	send(SCI_SETREADONLY, wasReadOnly ? 1 : 0);
	std::error_code statusError;
	monitoredSize = std::filesystem::file_size(path, statusError);
	monitoredTime = std::filesystem::last_write_time(path, statusError);
	return true;
}

bool EditorDocument::save(const std::filesystem::path &filePath, std::string &error)
{
	if (!writeCopy(filePath, error))
		return false;
	const std::filesystem::path target = std::filesystem::absolute(filePath).lexically_normal();
	path = target;
	displayName = path.filename().string();
	switch (send(SCI_GETEOLMODE))
	{
		case SC_EOL_CRLF:
			eolMode = EolMode::Windows;
			break;
		case SC_EOL_CR:
			eolMode = EolMode::MacClassic;
			break;
		default:
			eolMode = EolMode::Unix;
	}
	language = &languageForPath(path);
	send(SCI_SETSAVEPOINT);
	dirty = false;
	std::error_code statusError;
	monitoredSize = std::filesystem::file_size(path, statusError);
	monitoredTime = std::filesystem::last_write_time(path, statusError);
	return true;
}

bool EditorDocument::writeCopy(const std::filesystem::path &filePath, std::string &error) const
{
	std::string contents;
	if (!encodeText(text(), encoding, contents, error))
		return false;
	const std::filesystem::path target = std::filesystem::absolute(filePath).lexically_normal();
	std::error_code directoryError;
	if (!target.parent_path().empty() && !std::filesystem::exists(target.parent_path(), directoryError))
	{
		error = "A pasta de destino não existe: " + target.parent_path().string();
		return false;
	}
	const std::filesystem::path temporary = target.string() + ".npp-linux-tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			error = ioError(temporary);
			return false;
		}
		output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		output.close();
		if (!output)
		{
			error = "Não foi possível gravar o arquivo: " + target.string();
			std::filesystem::remove(temporary, directoryError);
			return false;
		}
	}
	std::error_code permissionsError;
	if (std::filesystem::exists(target))
	{
		const auto permissions = std::filesystem::status(target, permissionsError).permissions();
		if (!permissionsError)
			std::filesystem::permissions(temporary, permissions, permissionsError);
	}
	std::error_code renameError;
	std::filesystem::rename(temporary, target, renameError);
	// Preserve the old file if an atomic replacement fails.
	if (renameError)
	{
		error = "Não foi possível substituir o arquivo: " + renameError.message();
		std::filesystem::remove(temporary, directoryError);
		return false;
	}
	return true;
}

EolMode detectEol(const std::string &bytes)
{
	std::size_t lf = 0;
	std::size_t crlf = 0;
	std::size_t cr = 0;
	for (std::size_t i = 0; i < bytes.size(); ++i)
	{
		if (bytes[i] == '\n')
			++lf;
		if (bytes[i] == '\r')
		{
			if (i + 1 < bytes.size() && bytes[i + 1] == '\n')
				++crlf;
			else
				++cr;
		}
	}
	if (crlf > 0 && crlf >= lf - crlf)
		return EolMode::Windows;
	if (cr > lf)
		return EolMode::MacClassic;
	return EolMode::Unix;
}

const char *eolName(EolMode mode) noexcept
{
	switch (mode)
	{
		case EolMode::Windows:
			return "Windows (CRLF)";
		case EolMode::MacClassic:
			return "Mac clássico (CR)";
		default:
			return "Unix (LF)";
	}
}
}
