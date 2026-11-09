#ifndef NPP_LINUX_EDITOR_DOCUMENT_H
#define NPP_LINUX_EDITOR_DOCUMENT_H

#include "LanguageSupport.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <gtk/gtk.h>

#include "Scintilla.h"

namespace NppLinux
{
enum class EolMode
{
	Windows,
	Unix,
	MacClassic
};

class EditorDocument
{
public:
	explicit EditorDocument(GtkWidget *widget);
	~EditorDocument();

	EditorDocument(const EditorDocument &) = delete;
	EditorDocument &operator=(const EditorDocument &) = delete;

	GtkWidget *widget() const noexcept;
	sptr_t send(unsigned int message, uptr_t wParam = 0, sptr_t lParam = 0) const;
	void sendString(unsigned int message, uptr_t wParam, const std::string &value) const;

	std::string text() const;
	void setText(const std::string &text);
	// forcedEncoding reads the file as that encoding instead of detecting it.
	bool load(const std::filesystem::path &path, std::string &error, const std::string &forcedEncoding = {});
	bool save(const std::filesystem::path &path, std::string &error);
	// Writes the text in the document's encoding without changing the document.
	bool writeCopy(const std::filesystem::path &path, std::string &error) const;

	std::filesystem::path path;
	std::string recoveryId;
	std::string displayName;
	std::string encoding = "UTF-8";
	EolMode eolMode = EolMode::Unix;
	const Language *language = &plainTextLanguage();
	bool dirty = false;
	// Monitoring (tail -f): reload when the file grows, keep the view at the end.
	bool monitoring = false;
	// Edit → Begin/End Select: position marked by the first use, -1 when none.
	sptr_t selectionMark = -1;
	// The file was deleted or moved by another program and the person was told.
	bool missingNotified = false;
	std::uintmax_t monitoredSize = 0;
	std::filesystem::file_time_type monitoredTime{};

private:
	GtkWidget *_widget = nullptr;
};

EolMode detectEol(const std::string &bytes);
const char *eolName(EolMode mode) noexcept;
}

#endif
