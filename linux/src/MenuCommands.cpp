// Commands added to match the menus of the Windows version: file handling,
// text tools, bookmarks, highlighting, folding, encodings, tools, macros,
// run and window management.
#include "EditorWindow.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include <sys/stat.h>

#include <gio/gio.h>

#include "Scintilla.h"
#include "ScintillaWidget.h"
#include "TextTools.h"
#include "BuiltinTools.h"

namespace NppLinux
{
namespace
{
constexpr int BookmarkMarker = 20;
constexpr int BookmarkMask = 1 << BookmarkMarker;
constexpr int HistoryMask = (1 << SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN) | (1 << SC_MARKNUM_HISTORY_SAVED)
	| (1 << SC_MARKNUM_HISTORY_MODIFIED) | (1 << SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED);
constexpr int FirstTokenIndicator = 21;
constexpr int CompareRemovedMarker = 15;
constexpr int CompareAddedMarker = 16;
constexpr int CompareMask = (1 << CompareRemovedMarker) | (1 << CompareAddedMarker);
constexpr int TokenStyles = 5;

constexpr int bgr(int red, int green, int blue)
{
	return red | (green << 8) | (blue << 16);
}

void actionItemActivated(GtkMenuItem *item, gpointer data)
{
	const auto action = static_cast<Action>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "npp-action")));
	static_cast<EditorWindow *>(data)->perform(action);
}

GtkWidget *actionItem(GtkWidget *menu, const char *label, EditorWindow *window, Action action)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	g_object_set_data(G_OBJECT(item), "npp-action", GINT_TO_POINTER(static_cast<int>(action)));
	g_signal_connect(item, "activate", G_CALLBACK(actionItemActivated), window);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

std::string textRange(const EditorDocument &document, sptr_t start, sptr_t end)
{
	if (end <= start)
		return {};
	std::string text(static_cast<std::size_t>(end - start) + 1, '\0');
	Sci_TextRangeFull range{{start, end}, text.data()};
	document.send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&range));
	text.resize(static_cast<std::size_t>(end - start));
	return text;
}

std::string lineText(const EditorDocument &document, sptr_t line, bool withEol)
{
	const sptr_t start = document.send(SCI_POSITIONFROMLINE, line);
	const sptr_t end = withEol ? document.send(SCI_POSITIONFROMLINE, line + 1) : document.send(SCI_GETLINEENDPOSITION, line);
	return textRange(document, start, end < start ? document.send(SCI_GETLENGTH) : end);
}

std::string eolString(const EditorDocument &document)
{
	const int mode = static_cast<int>(document.send(SCI_GETEOLMODE));
	return mode == SC_EOL_CRLF ? "\r\n" : mode == SC_EOL_CR ? "\r" : "\n";
}

sptr_t currentLine(const EditorDocument &document)
{
	return document.send(SCI_LINEFROMPOSITION, document.send(SCI_GETCURRENTPOS));
}

void selectedLines(const EditorDocument &document, sptr_t &first, sptr_t &last)
{
	const sptr_t start = document.send(SCI_GETSELECTIONSTART);
	const sptr_t end = document.send(SCI_GETSELECTIONEND);
	first = document.send(SCI_LINEFROMPOSITION, start);
	last = document.send(SCI_LINEFROMPOSITION, end);
	if (last > first && document.send(SCI_POSITIONFROMLINE, last) == end)
		--last;
}

bool launchUri(const std::string &uri, std::string &error)
{
	GError *gerror = nullptr;
	if (g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &gerror))
		return true;
	error = gerror ? gerror->message : "não foi possível abrir";
	if (gerror)
		g_error_free(gerror);
	return false;
}

std::string fileUri(const std::filesystem::path &path)
{
	gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
	std::string result = uri ? uri : "";
	g_free(uri);
	return result;
}

std::string formatNow(const std::string &format)
{
	GDateTime *now = g_date_time_new_now_local();
	gchar *text = g_date_time_format(now, format.c_str());
	std::string result = text ? text : "";
	g_free(text);
	g_date_time_unref(now);
	return result;
}

std::string askText(GtkWindow *parent, const std::string &title, const std::string &caption, const std::string &initial)
{
	GtkWidget *dialog = gtk_dialog_new_with_buttons(title.c_str(), parent, GTK_DIALOG_MODAL,
		"_Cancelar", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_ACCEPT, nullptr);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	GtkWidget *label = gtk_label_new(caption.c_str());
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(entry), initial.c_str());
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_width_chars(GTK_ENTRY(entry), 48);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box);
	gtk_widget_show_all(dialog);
	std::string result;
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
		result = gtk_entry_get_text(GTK_ENTRY(entry));
	gtk_widget_destroy(dialog);
	return result;
}

bool confirm(GtkWindow *parent, const std::string &question, const std::string &detail, const char *accept)
{
	GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", question.c_str());
	if (!detail.empty())
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail.c_str());
	gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Cancelar", GTK_RESPONSE_CANCEL, accept, GTK_RESPONSE_ACCEPT, nullptr);
	const bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
	gtk_widget_destroy(dialog);
	return accepted;
}

std::filesystem::path chooseFile(GtkWindow *parent, const char *title, GtkFileChooserAction action,
	const char *accept, const std::filesystem::path &folder = {}, const std::string &name = {})
{
	GtkWidget *dialog = gtk_file_chooser_dialog_new(title, parent, action, "_Cancelar", GTK_RESPONSE_CANCEL, accept, GTK_RESPONSE_ACCEPT, nullptr);
	if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
		gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
	if (!folder.empty())
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), folder.c_str());
	if (!name.empty())
		gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), name.c_str());
	std::filesystem::path result;
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
	{
		gchar *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		if (file)
			result = file;
		g_free(file);
	}
	gtk_widget_destroy(dialog);
	return result;
}

struct IndicatorRange
{
	int indicator;
	sptr_t start;
	sptr_t end;
};

std::vector<IndicatorRange> tokenRanges(const EditorDocument &document)
{
	std::vector<IndicatorRange> ranges;
	const sptr_t length = document.send(SCI_GETLENGTH);
	// The five token styles and the mark of the Find dialog (indicator 26).
	for (int indicator = FirstTokenIndicator; indicator <= FirstTokenIndicator + TokenStyles; ++indicator)
	{
		sptr_t position = 0;
		while (position < length)
		{
			const sptr_t end = document.send(SCI_INDICATOREND, static_cast<uptr_t>(indicator), position);
			if (document.send(SCI_INDICATORVALUEAT, static_cast<uptr_t>(indicator), position))
				ranges.push_back({indicator, position, end > position ? end : length});
			if (end <= position)
				break;
			position = end;
		}
	}
	std::sort(ranges.begin(), ranges.end(), [](const IndicatorRange &a, const IndicatorRange &b) { return a.start < b.start; });
	return ranges;
}

// Printing: the text is laid out with Pango and split into pages.
struct PrintJob
{
	std::string text;
	std::string title;
	std::string font;
	PangoLayout *layout = nullptr;
	std::vector<int> pageFirstLine;
	std::vector<double> pageTop;
	std::vector<double> lineBaseline;
	double headerHeight = 0;
};

void beginPrint(GtkPrintOperation *operation, GtkPrintContext *context, gpointer data)
{
	auto *job = static_cast<PrintJob *>(data);
	job->layout = gtk_print_context_create_pango_layout(context);
	PangoFontDescription *description = pango_font_description_from_string(job->font.c_str());
	pango_layout_set_font_description(job->layout, description);
	pango_font_description_free(description);
	const double width = gtk_print_context_get_width(context);
	const double height = gtk_print_context_get_height(context);
	pango_layout_set_width(job->layout, static_cast<int>(width * PANGO_SCALE));
	pango_layout_set_wrap(job->layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_text(job->layout, job->text.c_str(), -1);
	job->headerHeight = 28;
	const double body = std::max(20.0, height - job->headerHeight);
	PangoLayoutIter *iter = pango_layout_get_iter(job->layout);
	int index = 0;
	job->pageFirstLine = {0};
	job->pageTop = {0};
	job->lineBaseline.clear();
	do
	{
		PangoRectangle logical;
		pango_layout_iter_get_line_extents(iter, nullptr, &logical);
		const double top = logical.y / static_cast<double>(PANGO_SCALE);
		const double bottom = (logical.y + logical.height) / static_cast<double>(PANGO_SCALE);
		if (bottom - job->pageTop.back() > body && index > job->pageFirstLine.back())
		{
			job->pageFirstLine.push_back(index);
			job->pageTop.push_back(top);
		}
		job->lineBaseline.push_back(pango_layout_iter_get_baseline(iter) / static_cast<double>(PANGO_SCALE));
		++index;
	} while (pango_layout_iter_next_line(iter));
	pango_layout_iter_free(iter);
	gtk_print_operation_set_n_pages(operation, static_cast<gint>(job->pageFirstLine.size()));
}

void drawPage(GtkPrintOperation *, GtkPrintContext *context, gint page, gpointer data)
{
	auto *job = static_cast<PrintJob *>(data);
	cairo_t *cairo = gtk_print_context_get_cairo_context(context);
	PangoLayout *header = gtk_print_context_create_pango_layout(context);
	PangoFontDescription *description = pango_font_description_from_string("Sans 8");
	pango_layout_set_font_description(header, description);
	pango_font_description_free(description);
	const std::string headerText = job->title + "   —   página " + std::to_string(page + 1) + " de " + std::to_string(job->pageFirstLine.size());
	pango_layout_set_text(header, headerText.c_str(), -1);
	cairo_set_source_rgb(cairo, 0.3, 0.3, 0.3);
	cairo_move_to(cairo, 0, 0);
	pango_cairo_show_layout(cairo, header);
	g_object_unref(header);
	cairo_set_source_rgb(cairo, 0, 0, 0);
	const std::size_t pageIndex = static_cast<std::size_t>(page);
	const int first = job->pageFirstLine[pageIndex];
	const int last = pageIndex + 1 < job->pageFirstLine.size() ? job->pageFirstLine[pageIndex + 1] : static_cast<int>(job->lineBaseline.size());
	for (int line = first; line < last; ++line)
	{
		PangoLayoutLine *layoutLine = pango_layout_get_line_readonly(job->layout, line);
		cairo_move_to(cairo, 0, job->headerHeight + job->lineBaseline[static_cast<std::size_t>(line)] - job->pageTop[pageIndex]);
		pango_cairo_show_layout_line(cairo, layoutLine);
	}
}

void endPrint(GtkPrintOperation *, GtkPrintContext *, gpointer data)
{
	auto *job = static_cast<PrintJob *>(data);
	if (job->layout)
		g_object_unref(job->layout);
	job->layout = nullptr;
}

void collectShortcuts(GtkWidget *menu, const std::string &path, std::vector<std::string> &rows)
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
	for (GList *entry = children; entry; entry = entry->next)
	{
		if (!GTK_IS_MENU_ITEM(entry->data) || GTK_IS_SEPARATOR_MENU_ITEM(entry->data))
			continue;
		GtkMenuItem *item = GTK_MENU_ITEM(entry->data);
		const char *raw = gtk_menu_item_get_label(item);
		std::string label = raw ? raw : "";
		label.erase(std::remove(label.begin(), label.end(), '_'), label.end());
		const std::string itemPath = path.empty() ? label : path + " → " + label;
		if (GtkWidget *sub = gtk_menu_item_get_submenu(item))
		{
			collectShortcuts(sub, itemPath, rows);
			continue;
		}
		// Shortcuts were added with gtk_widget_add_accelerator: look each closure up in its group.
		GList *closures = gtk_widget_list_accel_closures(GTK_WIDGET(item));
		for (GList *closure = closures; closure; closure = closure->next)
		{
			GtkAccelGroup *group = gtk_accel_group_from_accel_closure(static_cast<GClosure *>(closure->data));
			GtkAccelKey *found = group ? gtk_accel_group_find(group, +[](GtkAccelKey *, GClosure *candidate, gpointer data) -> gboolean {
				return candidate == data;
			}, closure->data) : nullptr;
			if (found && found->accel_key)
			{
				gchar *accel = gtk_accelerator_get_label(found->accel_key, found->accel_mods);
				std::string shortcut = accel ? accel : "";
				g_free(accel);
				shortcut.resize(std::max<std::size_t>(shortcut.size(), 22), ' ');
				rows.push_back(shortcut + "  " + itemPath);
			}
		}
		g_list_free(closures);
	}
	g_list_free(children);
}

std::string sanitizeName(const std::string &name)
{
	std::string result;
	for (const char c : name)
	{
		if (g_ascii_isalnum(c) || c == ' ' || c == '-' || c == '_' || static_cast<unsigned char>(c) >= 0x80)
			result += c;
	}
	while (!result.empty() && result.back() == ' ')
		result.pop_back();
	while (!result.empty() && result.front() == ' ')
		result.erase(result.begin());
	return result.substr(0, 80);
}

std::string osName()
{
	std::ifstream input("/etc/os-release");
	std::string line;
	while (std::getline(input, line))
	{
		if (line.rfind("PRETTY_NAME=", 0) == 0)
		{
			std::string value = line.substr(12);
			value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
			return value;
		}
	}
	return "Linux";
}
}

bool EditorWindow::quitKeepsUnsaved() const
{
	return _settings.rememberUnsaved && _settings.restoreSession && _sessionWritable && !_selfTesting;
}

void EditorWindow::copyToClipboard(const std::string &text)
{
	GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text(clipboard, text.c_str(), static_cast<gint>(text.size()));
	gtk_clipboard_store(clipboard);
}

void EditorWindow::showTextDialog(const std::string &title, const std::string &text)
{
	if (_selfTesting)
		return;
	GtkWidget *dialog = gtk_dialog_new_with_buttons(title.c_str(), GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"_Copiar", 1, "_Fechar", GTK_RESPONSE_CLOSE, nullptr);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 420);
	GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_container_set_border_width(GTK_CONTAINER(scroll), 8);
	GtkWidget *view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), text.c_str(), -1);
	gtk_container_add(GTK_CONTAINER(scroll), view);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 0);
	gtk_widget_show_all(dialog);
	while (gtk_dialog_run(GTK_DIALOG(dialog)) == 1)
	{
		copyToClipboard(text);
		setStatusMessage("Copiado para a área de transferência.");
	}
	gtk_widget_destroy(dialog);
}

std::string EditorWindow::currentWordOrSelection(EditorDocument &document) const
{
	const sptr_t start = document.send(SCI_GETSELECTIONSTART);
	const sptr_t end = document.send(SCI_GETSELECTIONEND);
	if (end > start)
		return textRange(document, start, end);
	const sptr_t position = document.send(SCI_GETCURRENTPOS);
	const sptr_t wordStart = document.send(SCI_WORDSTARTPOSITION, position, 1);
	const sptr_t wordEnd = document.send(SCI_WORDENDPOSITION, position, 1);
	return textRange(document, wordStart, wordEnd);
}

std::map<std::string, std::string> EditorWindow::runVariables(EditorDocument *document) const
{
	std::map<std::string, std::string> values;
	std::error_code error;
	const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
	values["NPP_DIRECTORY"] = executable.parent_path().string();
	values["NPP_FULL_FILE_PATH"] = executable.string();
	if (!document)
		return values;
	const auto &path = document->path;
	values["FULL_CURRENT_PATH"] = path.empty() ? document->displayName : path.string();
	values["CURRENT_DIRECTORY"] = path.empty() ? std::string() : path.parent_path().string();
	values["FILE_NAME"] = path.empty() ? document->displayName : path.filename().string();
	values["NAME_PART"] = path.empty() ? document->displayName : path.stem().string();
	values["EXT_PART"] = path.empty() ? std::string() : path.extension().string();
	values["CURRENT_WORD"] = currentWordOrSelection(*document);
	values["CURRENT_LINE"] = std::to_string(currentLine(*document) + 1);
	values["CURRENT_COLUMN"] = std::to_string(document->send(SCI_GETCOLUMN, document->send(SCI_GETCURRENTPOS)) + 1);
	values["CURRENT_LINESTR"] = lineText(*document, currentLine(*document), false);
	return values;
}

void EditorWindow::markModified(EditorDocument &document)
{
	if (document.send(SCI_GETREADONLY))
		return;
	// Scintilla only tracks changes to the text; an insert and delete in one
	// step marks the document as changed (for example after picking another
	// encoding) without altering it.
	document.send(SCI_BEGINUNDOACTION);
	document.send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>(" "));
	document.send(SCI_DELETERANGE, 0, 1);
	document.send(SCI_ENDUNDOACTION);
}

bool EditorWindow::convertEncoding(EditorDocument &document, const std::string &encoding)
{
	if (document.encoding == encoding)
		return true;
	std::string bytes;
	std::string error;
	if (!encodeText(document.text(), encoding, bytes, error))
	{
		showError(error);
		updateEncodingMenu();
		return false;
	}
	document.encoding = encoding;
	markModified(document);
	refreshDocument(&document);
	setStatusMessage("O documento será salvo em " + encodingLabel(encoding) + ".");
	return true;
}

bool EditorWindow::reopenWithEncoding(const std::string &encoding)
{
	EditorDocument *document = currentDocument();
	if (!document || document->path.empty())
	{
		setStatusMessage("Salve o documento antes de reabri-lo com outra codificação.");
		return false;
	}
	if (document->send(SCI_GETMODIFY) && !_selfTesting
		&& !confirm(GTK_WINDOW(_window), "Descartar as alterações não salvas?", "O arquivo será lido novamente do disco como " + encodingLabel(encoding) + ".", "_Reabrir"))
		return false;
	std::string error;
	const sptr_t position = document->send(SCI_GETCURRENTPOS);
	if (!document->load(document->path, error, encoding))
	{
		showError(error);
		return false;
	}
	applyTheme(*document);
	document->send(SCI_GOTOPOS, std::min(position, document->send(SCI_GETLENGTH)));
	_sessionStore.discardBackup(*document);
	refreshDocument(document);
	return true;
}

void EditorWindow::buildEncodingMenu(GtkWidget *menu)
{
	struct Callback
	{
		static void converted(GtkCheckMenuItem *item, gpointer data)
		{
			auto *window = static_cast<EditorWindow *>(data);
			if (window->_updating || !gtk_check_menu_item_get_active(item))
				return;
			const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(item), "npp-encoding"));
			if (EditorDocument *document = window->currentDocument())
				window->convertEncoding(*document, id);
		}
		static void reopened(GtkMenuItem *item, gpointer data)
		{
			const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(item), "npp-encoding"));
			static_cast<EditorWindow *>(data)->reopenWithEncoding(id);
		}
	};
	GSList *group = nullptr;
	auto addRadio = [&](GtkWidget *target, const EncodingInfo &info) {
		GtkWidget *item = gtk_radio_menu_item_new_with_label(group, info.label);
		group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
		g_object_set_data(G_OBJECT(item), "npp-encoding", const_cast<char *>(info.id));
		g_signal_connect(item, "toggled", G_CALLBACK(Callback::converted), this);
		gtk_menu_shell_append(GTK_MENU_SHELL(target), item);
		_encodingItems.emplace_back(item, info.id);
	};
	GtkWidget *title = gtk_menu_item_new_with_label("Salvar com a codificação:");
	gtk_widget_set_sensitive(title, FALSE);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), title);
	std::map<std::string, GtkWidget *> groups;
	GtkWidget *sets = gtk_menu_new();
	for (const auto &info : encodings())
	{
		if (!*info.group)
		{
			addRadio(menu, info);
			continue;
		}
		GtkWidget *&sub = groups[info.group];
		if (!sub)
		{
			sub = gtk_menu_new();
			GtkWidget *groupItem = gtk_menu_item_new_with_label(info.group);
			gtk_menu_item_set_submenu(GTK_MENU_ITEM(groupItem), sub);
			gtk_menu_shell_append(GTK_MENU_SHELL(sets), groupItem);
		}
		addRadio(sub, info);
	}
	GtkWidget *setsItem = gtk_menu_item_new_with_mnemonic("_Conjuntos de caracteres");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(setsItem), sets);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), setsItem);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	GtkWidget *reopen = gtk_menu_new();
	for (const auto &info : encodings())
	{
		const std::string label = *info.group ? std::string(info.group) + ": " + info.label : info.label;
		GtkWidget *item = gtk_menu_item_new_with_label(label.c_str());
		g_object_set_data(G_OBJECT(item), "npp-encoding", const_cast<char *>(info.id));
		g_signal_connect(item, "activate", G_CALLBACK(Callback::reopened), this);
		gtk_menu_shell_append(GTK_MENU_SHELL(reopen), item);
	}
	GtkWidget *reopenItem = gtk_menu_item_new_with_mnemonic("_Reabrir o arquivo como…");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(reopenItem), reopen);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), reopenItem);
}

void EditorWindow::updateEncodingMenu()
{
	const EditorDocument *document = currentDocument();
	if (!document)
		return;
	const bool wasUpdating = _updating;
	_updating = true;
	for (const auto &[item, id] : _encodingItems)
	{
		if (id == document->encoding)
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
	}
	_updating = wasUpdating;
}

void EditorWindow::applyMargins(EditorDocument &document)
{
	const StyleSpec &numbers = _theme[ThemeItem::LineNumber];
	document.send(SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
	document.send(SCI_SETMARGINMASKN, 1, BookmarkMask | HistoryMask);
	document.send(SCI_SETMARGINWIDTHN, 1, _settings.showBookmarkMargin ? 16 : 0);
	document.send(SCI_SETMARGINSENSITIVEN, 1, 1);
	document.send(SCI_SETMARGINCURSORN, 1, SC_CURSORARROW);
	document.send(SCI_MARKERDEFINE, BookmarkMarker, SC_MARK_BOOKMARK);
	document.send(SCI_MARKERSETFORE, BookmarkMarker, bgr(20, 60, 160));
	document.send(SCI_MARKERSETBACK, BookmarkMarker, bgr(70, 130, 230));
	const std::pair<int, int> history[] = {
		{SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN, bgr(64, 160, 191)},
		{SC_MARKNUM_HISTORY_SAVED, bgr(0, 160, 0)},
		{SC_MARKNUM_HISTORY_MODIFIED, bgr(255, 128, 0)},
		{SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED, bgr(160, 192, 0)}};
	for (const auto &[marker, colour] : history)
	{
		document.send(SCI_MARKERDEFINE, static_cast<uptr_t>(marker), SC_MARK_BAR);
		document.send(SCI_MARKERSETFORE, static_cast<uptr_t>(marker), colour);
		document.send(SCI_MARKERSETBACK, static_cast<uptr_t>(marker), colour);
	}
	const bool folding = _settings.showFoldMargin && !document.language->lexer.empty();
	document.send(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
	document.send(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
	document.send(SCI_SETMARGINWIDTHN, 2, folding ? 14 : 0);
	document.send(SCI_SETMARGINSENSITIVEN, 2, 1);
	document.send(SCI_SETFOLDMARGINCOLOUR, 1, numbers.back);
	document.send(SCI_SETFOLDMARGINHICOLOUR, 1, numbers.back);
	const std::pair<int, int> folders[] = {
		{SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS}, {SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS},
		{SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE}, {SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER},
		{SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED}, {SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED},
		{SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER}};
	for (const auto &[marker, symbol] : folders)
	{
		document.send(SCI_MARKERDEFINE, static_cast<uptr_t>(marker), symbol);
		document.send(SCI_MARKERSETFORE, static_cast<uptr_t>(marker), numbers.back);
		document.send(SCI_MARKERSETBACK, static_cast<uptr_t>(marker), numbers.fore);
	}
	document.send(SCI_MARKERDEFINE, CompareRemovedMarker, SC_MARK_BACKGROUND);
	document.send(SCI_MARKERSETBACK, CompareRemovedMarker, _settings.darkTheme ? bgr(100, 40, 40) : bgr(255, 205, 205));
	document.send(SCI_MARKERDEFINE, CompareAddedMarker, SC_MARK_BACKGROUND);
	document.send(SCI_MARKERSETBACK, CompareAddedMarker, _settings.darkTheme ? bgr(35, 85, 40) : bgr(205, 245, 205));
	document.send(SCI_INDICSETSTYLE, FirstTokenIndicator + TokenStyles, INDIC_ROUNDBOX);
	document.send(SCI_INDICSETFORE, FirstTokenIndicator + TokenStyles, bgr(255, 0, 0));
	document.send(SCI_INDICSETALPHA, FirstTokenIndicator + TokenStyles, 90);
	document.send(SCI_INDICSETOUTLINEALPHA, FirstTokenIndicator + TokenStyles, 180);
	document.send(SCI_INDICSETUNDER, FirstTokenIndicator + TokenStyles, 1);
	const int tokenColours[TokenStyles] = {bgr(0, 255, 255), bgr(255, 128, 0), bgr(255, 255, 0), bgr(128, 0, 255), bgr(0, 128, 0)};
	for (int i = 0; i < TokenStyles; ++i)
	{
		const auto indicator = static_cast<uptr_t>(FirstTokenIndicator + i);
		document.send(SCI_INDICSETSTYLE, indicator, INDIC_ROUNDBOX);
		document.send(SCI_INDICSETFORE, indicator, tokenColours[i]);
		document.send(SCI_INDICSETALPHA, indicator, 100);
		document.send(SCI_INDICSETOUTLINEALPHA, indicator, 160);
		document.send(SCI_INDICSETUNDER, indicator, 1);
	}
}

bool EditorWindow::toggleBookmark(EditorDocument &document, sptr_t line)
{
	if (document.send(SCI_MARKERGET, line) & BookmarkMask)
	{
		document.send(SCI_MARKERDELETE, line, BookmarkMarker);
		return false;
	}
	document.send(SCI_MARKERADD, line, BookmarkMarker);
	return true;
}

std::size_t EditorWindow::bookmarkedLineCount(EditorDocument &document)
{
	std::size_t count = 0;
	for (sptr_t line = document.send(SCI_MARKERNEXT, 0, BookmarkMask); line >= 0; line = document.send(SCI_MARKERNEXT, line + 1, BookmarkMask))
		++count;
	return count;
}

void EditorWindow::marginClicked(EditorDocument *document, int margin, sptr_t position)
{
	if (document && margin == 1)
		toggleBookmark(*document, document->send(SCI_LINEFROMPOSITION, position));
}

bool EditorWindow::styleAllOccurrences(EditorDocument &document, int style)
{
	const std::string token = currentWordOrSelection(document);
	if (token.empty())
		return false;
	const sptr_t selectionStart = document.send(SCI_GETSELECTIONSTART);
	const sptr_t selectionEnd = document.send(SCI_GETSELECTIONEND);
	const bool selection = selectionEnd > selectionStart;
	document.send(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(FirstTokenIndicator + style));
	document.send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE | (selection ? 0 : SCFIND_WHOLEWORD));
	const sptr_t length = document.send(SCI_GETLENGTH);
	sptr_t start = 0;
	std::size_t count = 0;
	while (start < length)
	{
		document.send(SCI_SETTARGETRANGE, start, length);
		const sptr_t found = document.send(SCI_SEARCHINTARGET, token.size(), reinterpret_cast<sptr_t>(token.c_str()));
		if (found < 0)
			break;
		const sptr_t end = document.send(SCI_GETTARGETEND);
		document.send(SCI_INDICATORFILLRANGE, found, end - found);
		++count;
		start = end > found ? end : found + 1;
	}
	setStatusMessage(std::to_string(count) + " ocorrência(s) destacada(s).");
	return count > 0;
}

void EditorWindow::foldLevel(EditorDocument &document, int level, bool fold)
{
	document.send(SCI_COLOURISE, 0, -1);
	const sptr_t lines = document.send(SCI_GETLINECOUNT);
	for (sptr_t line = 0; line < lines; ++line)
	{
		const sptr_t value = document.send(SCI_GETFOLDLEVEL, line);
		if ((value & SC_FOLDLEVELHEADERFLAG) && ((value & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == level - 1)
			document.send(SCI_FOLDLINE, line, fold ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
	}
}

void EditorWindow::moveTab(std::size_t from, std::size_t to)
{
	if (from >= _documents.size() || to >= _documents.size() || from == to)
		return;
	GtkWidget *widget = _documents[from]->widget();
	gtk_notebook_reorder_child(GTK_NOTEBOOK(_notebook), widget, static_cast<gint>(to));
	setCurrentPage(gtk_notebook_page_num(GTK_NOTEBOOK(_notebook), widget));
}

void EditorWindow::sortTabs(Action order)
{
	std::vector<EditorDocument *> sorted;
	for (const auto &document : _documents)
		sorted.push_back(document.get());
	auto key = [&](const EditorDocument *document) -> std::string {
		switch (order)
		{
			case Action::SortTabsPathAsc: case Action::SortTabsPathDesc:
				return document->path.empty() ? document->displayName : document->path.string();
			case Action::SortTabsTypeAsc: case Action::SortTabsTypeDesc:
				return (document->path.empty() ? std::string() : document->path.extension().string()) + "\x01" + document->displayName;
			case Action::SortTabsSizeAsc: case Action::SortTabsSizeDesc:
			{
				const std::string size = std::to_string(document->send(SCI_GETLENGTH));
				return std::string(20 - std::min<std::size_t>(20, size.size()), '0') + size;
			}
			case Action::SortTabsTimeAsc: case Action::SortTabsTimeDesc:
			{
				struct stat info{};
				const long long seconds = !document->path.empty() && stat(document->path.c_str(), &info) == 0 ? static_cast<long long>(info.st_mtime) : 0;
				const std::string time = std::to_string(seconds);
				return std::string(20 - std::min<std::size_t>(20, time.size()), '0') + time;
			}
			default:
				return document->displayName;
		}
	};
	const bool descending = order == Action::SortTabsNameDesc || order == Action::SortTabsPathDesc
		|| order == Action::SortTabsTypeDesc || order == Action::SortTabsSizeDesc || order == Action::SortTabsTimeDesc;
	std::stable_sort(sorted.begin(), sorted.end(), [&](const EditorDocument *a, const EditorDocument *b) {
		gchar *left = g_utf8_casefold(key(a).c_str(), -1);
		gchar *right = g_utf8_casefold(key(b).c_str(), -1);
		const int compared = g_strcmp0(left, right);
		g_free(left);
		g_free(right);
		return descending ? compared > 0 : compared < 0;
	});
	EditorDocument *current = currentDocument();
	for (std::size_t i = 0; i < sorted.size(); ++i)
		gtk_notebook_reorder_child(GTK_NOTEBOOK(_notebook), sorted[i]->widget(), static_cast<gint>(i));
	if (current)
		setCurrentPage(gtk_notebook_page_num(GTK_NOTEBOOK(_notebook), current->widget()));
}

void EditorWindow::closeMultiple(Action which)
{
	const std::size_t current = currentIndex();
	for (std::size_t i = _documents.size(); i-- > 0;)
	{
		if (i >= _documents.size())
			continue;
		bool close = false;
		switch (which)
		{
			case Action::CloseAll: close = true; break;
			case Action::CloseAllButActive: close = i != current; break;
			case Action::CloseAllToLeft: close = i < current; break;
			case Action::CloseAllToRight: close = i > current; break;
			case Action::CloseAllUnchanged: close = !_documents[i]->send(SCI_GETMODIFY); break;
			default: break;
		}
		if (!close)
			continue;
		setCurrentPage(static_cast<int>(i));
		if (!closeDocument(i, true))
			break;
	}
}

bool EditorWindow::openSessionFile(const std::filesystem::path &file)
{
	const auto paths = SessionStore::loadSessionFile(file);
	std::size_t opened = 0;
	for (const auto &path : paths)
	{
		std::error_code error;
		if (std::filesystem::is_regular_file(path, error))
		{
			openPath(path);
			++opened;
		}
	}
	setStatusMessage(std::to_string(opened) + " de " + std::to_string(paths.size()) + " arquivo(s) da sessão aberto(s).");
	return !paths.empty();
}

bool EditorWindow::saveSessionFileAs(const std::filesystem::path &file)
{
	std::vector<std::string> paths;
	for (const auto &document : _documents)
	{
		if (!document->path.empty())
			paths.push_back(document->path.string());
	}
	return SessionStore::saveSessionFile(file, paths);
}

void EditorWindow::printDocument(bool showDialog)
{
	EditorDocument *document = currentDocument();
	if (!document || _selfTesting)
		return;
	PrintJob job;
	job.text = document->text();
	job.title = document->path.empty() ? document->displayName : document->path.string();
	job.font = _settings.fontFamily + " 9";
	GtkPrintOperation *operation = gtk_print_operation_new();
	gtk_print_operation_set_job_name(operation, document->displayName.c_str());
	gtk_print_operation_set_unit(operation, GTK_UNIT_POINTS);
	gtk_print_operation_set_embed_page_setup(operation, TRUE);
	g_signal_connect(operation, "begin-print", G_CALLBACK(beginPrint), &job);
	g_signal_connect(operation, "draw-page", G_CALLBACK(drawPage), &job);
	g_signal_connect(operation, "end-print", G_CALLBACK(endPrint), &job);
	GError *error = nullptr;
	const GtkPrintOperationResult result = gtk_print_operation_run(operation,
		showDialog ? GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG : GTK_PRINT_OPERATION_ACTION_PRINT, GTK_WINDOW(_window), &error);
	if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
	{
		showError(std::string("Não foi possível imprimir.\n\n") + (error ? error->message : ""));
		if (error)
			g_error_free(error);
	}
	else if (result == GTK_PRINT_OPERATION_RESULT_APPLY)
		setStatusMessage("Documento enviado para impressão.");
	g_object_unref(operation);
}

void EditorWindow::monitorTick()
{
	if (_window && gtk_window_is_active(GTK_WINDOW(_window)))
		checkExternalChanges(false);
	for (const auto &owned : _documents)
	{
		EditorDocument &document = *owned;
		if (!document.monitoring || document.path.empty())
			continue;
		std::error_code error;
		const auto size = std::filesystem::file_size(document.path, error);
		const auto time = error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(document.path, error);
		if (error)
		{
			document.monitoring = false;
			document.send(SCI_SETREADONLY, 0);
			setStatusMessage("Monitoramento parado: o arquivo não está mais disponível.");
			continue;
		}
		if (size == document.monitoredSize && time == document.monitoredTime)
			continue;
		std::string loadError;
		if (document.load(document.path, loadError, document.encoding))
		{
			applyTheme(document);
			document.send(SCI_DOCUMENTEND);
			refreshDocument(&document);
		}
	}
}

bool EditorWindow::reloadFromDisk(EditorDocument &document, bool ask)
{
	if (document.path.empty())
		return false;
	if (ask && document.send(SCI_GETMODIFY) && !_selfTesting
		&& !confirm(GTK_WINDOW(_window), "Recarregar “" + document.displayName + "” do disco?", "As alterações não salvas serão perdidas.", "_Recarregar"))
		return false;
	const sptr_t position = document.send(SCI_GETCURRENTPOS);
	std::string error;
	if (!document.load(document.path, error))
	{
		showError(error);
		return false;
	}
	applyTheme(document);
	document.send(SCI_GOTOPOS, std::min(position, document.send(SCI_GETLENGTH)));
	_sessionStore.discardBackup(document);
	refreshDocument(&document);
	return true;
}

bool EditorWindow::runMacroCommands(const std::vector<MacroCommand> &commands, std::size_t repetitions, bool untilEnd)
{
	if (_recordingMacro || _playingMacro || commands.empty())
		return false;
	if (std::any_of(commands.begin(), commands.end(), [](const MacroCommand &command) { return !SessionStore::isAllowed(command); }))
		return false;
	EditorDocument *document = currentDocument();
	if (!document)
		return false;
	const auto function = reinterpret_cast<SciFnDirect>(document->send(SCI_GETDIRECTFUNCTION));
	const sptr_t pointer = document->send(SCI_GETDIRECTPOINTER);
	if (!function || !pointer)
		return false;
	_playingMacro = true;
	document->send(SCI_BEGINUNDOACTION);
	const std::size_t limit = untilEnd ? 1000000 : std::min<std::size_t>(repetitions, 100000);
	const gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
	for (std::size_t repetition = 0; repetition < limit; ++repetition)
	{
		const sptr_t before = document->send(SCI_GETCURRENTPOS);
		const sptr_t lengthBefore = document->send(SCI_GETLENGTH);
		for (const auto &command : commands)
			function(pointer, command.message, command.wParam, command.hasText ? reinterpret_cast<sptr_t>(command.text.c_str()) : command.lParam);
		if (untilEnd)
		{
			const sptr_t after = document->send(SCI_GETCURRENTPOS);
			// Stop at the end of the document or when the macro no longer moves forward.
			if (after >= document->send(SCI_GETLENGTH) || (after <= before && document->send(SCI_GETLENGTH) == lengthBefore))
				break;
		}
		if (g_get_monotonic_time() > deadline)
		{
			setStatusMessage("A macro foi interrompida depois de 30 segundos.");
			break;
		}
	}
	document->send(SCI_ENDUNDOACTION);
	document->send(SCI_SCROLLCARET);
	_playingMacro = false;
	updateMacroState();
	refreshDocument(document);
	return true;
}

bool EditorWindow::runMacroFile(const std::filesystem::path &file, std::size_t repetitions, bool untilEnd)
{
	const auto commands = SessionStore::loadMacroFile(file);
	if (commands.empty())
	{
		showError("A macro “" + file.stem().string() + "” está vazia ou não pôde ser lida.");
		return false;
	}
	return runMacroCommands(commands, repetitions, untilEnd);
}

void EditorWindow::rebuildMacroMenu()
{
	if (!_macroMenu)
		return;
	GList *children = gtk_container_get_children(GTK_CONTAINER(_macroMenu));
	for (GList *entry = children; entry; entry = entry->next)
	{
		if (g_object_get_data(G_OBJECT(entry->data), "npp-dynamic"))
			gtk_widget_destroy(GTK_WIDGET(entry->data));
	}
	g_list_free(children);
	std::vector<std::filesystem::path> files;
	std::error_code error;
	for (std::filesystem::directory_iterator it(_sessionStore.macroDirectory(), error), end; !error && it != end; it.increment(error))
	{
		if (it->path().extension() == ".xml")
			files.push_back(it->path());
	}
	std::sort(files.begin(), files.end());
	if (files.empty())
		return;
	GtkWidget *separator = gtk_separator_menu_item_new();
	g_object_set_data(G_OBJECT(separator), "npp-dynamic", GINT_TO_POINTER(1));
	gtk_menu_shell_append(GTK_MENU_SHELL(_macroMenu), separator);
	for (const auto &file : files)
	{
		GtkWidget *item = gtk_menu_item_new_with_label(file.stem().c_str());
		g_object_set_data(G_OBJECT(item), "npp-dynamic", GINT_TO_POINTER(1));
		g_object_set_data_full(G_OBJECT(item), "npp-macro-file", g_strdup(file.c_str()), g_free);
		g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem *menuItem, gpointer data) {
			const auto *path = static_cast<const char *>(g_object_get_data(G_OBJECT(menuItem), "npp-macro-file"));
			static_cast<EditorWindow *>(data)->runMacroFile(path, 1, false);
		}), this);
		gtk_menu_shell_append(GTK_MENU_SHELL(_macroMenu), item);
	}
	gtk_widget_show_all(_macroMenu);
}

void EditorWindow::rebuildWindowMenu()
{
	if (!_windowMenu)
		return;
	GList *children = gtk_container_get_children(GTK_CONTAINER(_windowMenu));
	for (GList *entry = children; entry; entry = entry->next)
		gtk_widget_destroy(GTK_WIDGET(entry->data));
	g_list_free(children);
	actionItem(_windowMenu, "_Janelas…", this, Action::WindowsList);
	GtkWidget *sort = gtk_menu_new();
	actionItem(sort, "Nome de A a Z", this, Action::SortTabsNameAsc);
	actionItem(sort, "Nome de Z a A", this, Action::SortTabsNameDesc);
	actionItem(sort, "Caminho de A a Z", this, Action::SortTabsPathAsc);
	actionItem(sort, "Caminho de Z a A", this, Action::SortTabsPathDesc);
	actionItem(sort, "Tipo de A a Z", this, Action::SortTabsTypeAsc);
	actionItem(sort, "Tipo de Z a A", this, Action::SortTabsTypeDesc);
	actionItem(sort, "Tamanho crescente", this, Action::SortTabsSizeAsc);
	actionItem(sort, "Tamanho decrescente", this, Action::SortTabsSizeDesc);
	actionItem(sort, "Modificado há mais tempo primeiro", this, Action::SortTabsTimeAsc);
	actionItem(sort, "Modificado recentemente primeiro", this, Action::SortTabsTimeDesc);
	GtkWidget *sortItem = gtk_menu_item_new_with_mnemonic("_Ordenar abas por");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(sortItem), sort);
	gtk_menu_shell_append(GTK_MENU_SHELL(_windowMenu), sortItem);
	if (!_documents.empty())
		gtk_menu_shell_append(GTK_MENU_SHELL(_windowMenu), gtk_separator_menu_item_new());
	const std::size_t current = _notebook ? currentIndex() : 0;
	for (std::size_t i = 0; i < _documents.size() && i < 20; ++i)
	{
		const std::string label = std::to_string(i + 1) + ": " + (_documents[i]->send(SCI_GETMODIFY) ? "● " : "") + _documents[i]->displayName;
		GtkWidget *item = gtk_check_menu_item_new_with_label(label.c_str());
		gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), TRUE);
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), i == current);
		g_object_set_data(G_OBJECT(item), "npp-page", GINT_TO_POINTER(static_cast<int>(i)));
		g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem *menuItem, gpointer data) {
			auto *window = static_cast<EditorWindow *>(data);
			const int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuItem), "npp-page"));
			window->performMore(static_cast<Action>(static_cast<int>(Action::Tab1) + std::min(page, 8)));
			if (page > 8)
				window->goToPage(page);
		}), this);
		gtk_menu_shell_append(GTK_MENU_SHELL(_windowMenu), item);
	}
	gtk_widget_show_all(_windowMenu);
}

void EditorWindow::goToPage(int page)
{
	if (page >= 0 && static_cast<std::size_t>(page) < _documents.size())
		setCurrentPage(page);
}

void EditorWindow::showWindowsList()
{
	if (_selfTesting)
		return;
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Janelas", GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"_Ativar", 1, "_Salvar", 2, "Fec_har documento", 3, "_Fechar", GTK_RESPONSE_CLOSE, nullptr);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 420);
	GtkListStore *store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
	GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);
	const char *titles[] = {"Nome", "Pasta", "Tipo", "Tamanho"};
	for (int column = 0; column < 4; ++column)
	{
		GtkTreeViewColumn *treeColumn = gtk_tree_view_column_new_with_attributes(titles[column], gtk_cell_renderer_text_new(), "text", column, nullptr);
		gtk_tree_view_column_set_resizable(treeColumn, TRUE);
		gtk_tree_view_column_set_sort_column_id(treeColumn, column);
		gtk_tree_view_append_column(GTK_TREE_VIEW(view), treeColumn);
	}
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), GTK_SELECTION_MULTIPLE);
	auto fill = [&]() {
		gtk_list_store_clear(store);
		for (const auto &document : _documents)
		{
			GtkTreeIter iter;
			gtk_list_store_append(store, &iter);
			const std::string name = (document->send(SCI_GETMODIFY) ? "● " : "") + document->displayName;
			const std::string size = std::to_string(document->send(SCI_GETLENGTH)) + " bytes";
			const std::string folder = document->path.empty() ? std::string("(não salvo)") : document->path.parent_path().string();
			gtk_list_store_set(store, &iter, 0, name.c_str(), 1, folder.c_str(),
				2, document->language->name.c_str(), 3, size.c_str(), 4, document.get(), -1);
		}
	};
	fill();
	GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_container_set_border_width(GTK_CONTAINER(scroll), 8);
	gtk_container_add(GTK_CONTAINER(scroll), view);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 0);
	gtk_widget_show_all(dialog);
	auto selected = [&]() {
		std::vector<EditorDocument *> documents;
		GtkTreeModel *model = nullptr;
		GList *rows = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model);
		for (GList *row = rows; row; row = row->next)
		{
			GtkTreeIter iter;
			if (gtk_tree_model_get_iter(model, &iter, static_cast<GtkTreePath *>(row->data)))
			{
				gpointer pointer = nullptr;
				gtk_tree_model_get(model, &iter, 4, &pointer, -1);
				documents.push_back(static_cast<EditorDocument *>(pointer));
			}
		}
		g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
		return documents;
	};
	auto indexOf = [&](EditorDocument *document) -> int {
		for (std::size_t i = 0; i < _documents.size(); ++i)
		{
			if (_documents[i].get() == document)
				return static_cast<int>(i);
		}
		return -1;
	};
	for (;;)
	{
		const int response = gtk_dialog_run(GTK_DIALOG(dialog));
		if (response != 1 && response != 2 && response != 3)
			break;
		const auto documents = selected();
		for (EditorDocument *document : documents)
		{
			const int index = indexOf(document);
			if (index < 0)
				continue;
			if (response == 1)
				setCurrentPage(index);
			else if (response == 2)
				saveDocument(*document);
			else if (!closeDocument(static_cast<std::size_t>(index)))
				break;
		}
		if (response == 1)
			break;
		fill();
	}
	gtk_widget_destroy(dialog);
}

void EditorWindow::showHashDialog(int kind, bool files)
{
	const auto hash = static_cast<HashKind>(kind);
	const std::string name = hashName(hash);
	if (_selfTesting)
		return;
	if (files)
	{
		GtkWidget *chooser = gtk_file_chooser_dialog_new(("Gerar " + name + " de arquivos").c_str(), GTK_WINDOW(_window),
			GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancelar", GTK_RESPONSE_CANCEL, "_Gerar", GTK_RESPONSE_ACCEPT, nullptr);
		gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
		std::string result;
		if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
		{
			GSList *list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(chooser));
			for (GSList *entry = list; entry; entry = entry->next)
			{
				const std::filesystem::path path = static_cast<const char *>(entry->data);
				std::ifstream input(path, std::ios::binary);
				std::ostringstream buffer;
				buffer << input.rdbuf();
				result += (input ? hashHex(hash, buffer.str()) : std::string("(não foi possível ler)")) + "  " + path.filename().string() + "\n";
				g_free(entry->data);
			}
			g_slist_free(list);
		}
		gtk_widget_destroy(chooser);
		if (!result.empty())
			showTextDialog(name, result);
		return;
	}
	GtkWidget *dialog = gtk_dialog_new_with_buttons(("Gerar " + name).c_str(), GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"_Copiar resultado", 1, "_Fechar", GTK_RESPONSE_CLOSE, nullptr);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 360);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(box), 10);
	GtkWidget *caption = gtk_label_new("Digite ou cole o texto:");
	gtk_label_set_xalign(GTK_LABEL(caption), 0.0);
	GtkWidget *input = gtk_text_view_new();
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(input), TRUE);
	GtkWidget *inputScroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_container_add(GTK_CONTAINER(inputScroll), input);
	GtkWidget *perLine = gtk_check_button_new_with_label("Tratar cada linha como um texto separado");
	GtkWidget *output = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(output), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(output), TRUE);
	GtkWidget *outputScroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_container_add(GTK_CONTAINER(outputScroll), output);
	gtk_box_pack_start(GTK_BOX(box), caption, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), inputScroll, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), perLine, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), outputScroll, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE, 0);
	struct State
	{
		HashKind kind;
		GtkWidget *input;
		GtkWidget *output;
		GtkWidget *perLine;
	};
	State state{hash, input, output, perLine};
	auto update = +[](gpointer data) {
		auto *s = static_cast<State *>(data);
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->input));
		GtkTextIter start;
		GtkTextIter end;
		gtk_text_buffer_get_bounds(buffer, &start, &end);
		gchar *raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
		const std::string text = raw ? raw : "";
		g_free(raw);
		std::string result;
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->perLine)))
		{
			std::istringstream lines(text);
			std::string line;
			while (std::getline(lines, line))
				result += hashHex(s->kind, line) + "  " + line + "\n";
		}
		else
			result = hashHex(s->kind, text);
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->output)), result.c_str(), -1);
	};
	g_signal_connect_swapped(gtk_text_view_get_buffer(GTK_TEXT_VIEW(input)), "changed", G_CALLBACK(update), &state);
	g_signal_connect_swapped(perLine, "toggled", G_CALLBACK(update), &state);
	update(&state);
	gtk_widget_show_all(dialog);
	while (gtk_dialog_run(GTK_DIALOG(dialog)) == 1)
	{
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(output));
		GtkTextIter start;
		GtkTextIter end;
		gtk_text_buffer_get_bounds(buffer, &start, &end);
		gchar *raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
		copyToClipboard(raw ? raw : "");
		g_free(raw);
	}
	gtk_widget_destroy(dialog);
}

void EditorWindow::showColumnEditor()
{
	EditorDocument *document = currentDocument();
	if (!document || _selfTesting)
		return;
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Editor de colunas", GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"_Cancelar", GTK_RESPONSE_CANCEL, "_Inserir", GTK_RESPONSE_ACCEPT, nullptr);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
	GtkWidget *textMode = gtk_radio_button_new_with_label(nullptr, "Texto a inserir:");
	GtkWidget *text = gtk_entry_new();
	GtkWidget *numberMode = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(textMode), "Número a inserir");
	GtkWidget *initial = gtk_spin_button_new_with_range(-1e9, 1e9, 1);
	GtkWidget *step = gtk_spin_button_new_with_range(-1e9, 1e9, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(step), 1);
	GtkWidget *repeat = gtk_spin_button_new_with_range(1, 1e6, 1);
	GtkWidget *leading = gtk_combo_box_text_new();
	for (const char *option : {"Nenhum", "Zeros", "Espaços"})
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(leading), option);
	gtk_combo_box_set_active(GTK_COMBO_BOX(leading), 0);
	GtkWidget *format = gtk_combo_box_text_new();
	for (const char *option : {"Decimal", "Hexadecimal", "Octal", "Binário"})
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(format), option);
	gtk_combo_box_set_active(GTK_COMBO_BOX(format), 0);
	auto row = [&](int y, const char *caption, GtkWidget *widget) {
		GtkWidget *label = gtk_label_new(caption);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_grid_attach(GTK_GRID(grid), label, 0, y, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), widget, 1, y, 1, 1);
	};
	gtk_grid_attach(GTK_GRID(grid), textMode, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), text, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), numberMode, 0, 1, 2, 1);
	row(2, "    Número inicial:", initial);
	row(3, "    Aumentar em:", step);
	row(4, "    Repetir cada número:", repeat);
	row(5, "    Preencher à esquerda com:", leading);
	row(6, "    Formato:", format);
	GtkWidget *note = gtk_label_new("Insere na coluna do cursor, da linha atual até o fim da seleção (ou do documento).");
	gtk_label_set_xalign(GTK_LABEL(note), 0.0);
	gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
	gtk_grid_attach(GTK_GRID(grid), note, 0, 7, 2, 1);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid);
	gtk_widget_show_all(dialog);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
	{
		ColumnInsert insert;
		insert.numbers = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(numberMode));
		insert.text = gtk_entry_get_text(GTK_ENTRY(text));
		insert.initial = static_cast<long long>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(initial)));
		insert.increment = static_cast<long long>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(step)));
		insert.repeat = std::max<long long>(1, static_cast<long long>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(repeat))));
		insert.padding = gtk_combo_box_get_active(GTK_COMBO_BOX(leading));
		insert.base = gtk_combo_box_get_active(GTK_COMBO_BOX(format));
		insertColumn(*document, insert);
	}
	gtk_widget_destroy(dialog);
}

void EditorWindow::insertColumn(EditorDocument &document, const ColumnInsert &insert)
{
	const sptr_t caret = document.send(SCI_GETSELECTIONSTART);
	const sptr_t selectionEnd = document.send(SCI_GETSELECTIONEND);
	const sptr_t firstLine = document.send(SCI_LINEFROMPOSITION, caret);
	const sptr_t lastLine = selectionEnd > caret && document.send(SCI_LINEFROMPOSITION, selectionEnd) > firstLine
		? document.send(SCI_LINEFROMPOSITION, selectionEnd) : document.send(SCI_GETLINECOUNT) - 1;
	const sptr_t column = document.send(SCI_GETCOLUMN, caret);
	auto formatNumber = [&](long long value) {
		std::string digits;
		const bool negative = value < 0;
		unsigned long long magnitude = negative ? static_cast<unsigned long long>(-value) : static_cast<unsigned long long>(value);
		const unsigned radix = insert.base == 1 ? 16 : insert.base == 2 ? 8 : insert.base == 3 ? 2 : 10;
		do
		{
			digits.insert(digits.begin(), "0123456789ABCDEF"[magnitude % radix]);
			magnitude /= radix;
		} while (magnitude);
		return (negative ? "-" : "") + digits;
	};
	std::vector<std::string> values;
	std::size_t width = 0;
	for (sptr_t line = firstLine; line <= lastLine; ++line)
	{
		const long long index = (line - firstLine) / std::max<long long>(1, insert.repeat);
		values.push_back(insert.numbers ? formatNumber(insert.initial + index * insert.increment) : insert.text);
		width = std::max(width, values.back().size());
	}
	document.send(SCI_BEGINUNDOACTION);
	for (sptr_t line = lastLine; line >= firstLine; --line)
	{
		std::string value = values[static_cast<std::size_t>(line - firstLine)];
		if (insert.numbers && insert.padding > 0 && value.size() < width)
			value.insert(0, width - value.size(), insert.padding == 1 ? '0' : ' ');
		const sptr_t position = document.send(SCI_FINDCOLUMN, line, column);
		const sptr_t reached = document.send(SCI_GETCOLUMN, position);
		if (reached < column)
			value.insert(0, static_cast<std::size_t>(column - reached), ' ');
		document.send(SCI_INSERTTEXT, position, reinterpret_cast<sptr_t>(value.c_str()));
	}
	document.send(SCI_ENDUNDOACTION);
}

void EditorWindow::showRunDialog()
{
	if (_selfTesting)
		return;
	EditorDocument *document = currentDocument();
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Executar", GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"_Cancelar", GTK_RESPONSE_CANCEL, "_Executar", GTK_RESPONSE_ACCEPT, nullptr);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	GtkWidget *caption = gtk_label_new("Programa ou comando a executar:");
	gtk_label_set_xalign(GTK_LABEL(caption), 0.0);
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_width_chars(GTK_ENTRY(entry), 60);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_text(GTK_ENTRY(entry), _settings.lastRunCommand.c_str());
	GtkWidget *help = gtk_label_new(
		"Variáveis: $(FULL_CURRENT_PATH)  $(CURRENT_DIRECTORY)  $(FILE_NAME)  $(NAME_PART)  $(EXT_PART)\n"
		"$(CURRENT_WORD)  $(CURRENT_LINE)  $(CURRENT_COLUMN)  $(CURRENT_LINESTR)  $(NPP_DIRECTORY)\n"
		"Exemplo: xdg-open \"$(FULL_CURRENT_PATH)\"  —  use aspas em caminhos com espaços.\n"
		"O comando roda pelo sh do sistema, na pasta do arquivo atual.");
	gtk_label_set_xalign(GTK_LABEL(help), 0.0);
	gtk_label_set_selectable(GTK_LABEL(help), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(help), "dim-label");
	gtk_box_pack_start(GTK_BOX(box), caption, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), help, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box);
	gtk_widget_show_all(dialog);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
	{
		const std::string command = gtk_entry_get_text(GTK_ENTRY(entry));
		if (!command.empty())
		{
			_settings.lastRunCommand = command;
			saveSettings();
			const auto values = runVariables(document);
			const std::string expanded = expandVariables(command, values);
			std::string directory = values.count("CURRENT_DIRECTORY") ? values.at("CURRENT_DIRECTORY") : std::string();
			if (directory.empty())
				directory = g_get_home_dir();
			gchar *argv[] = {const_cast<gchar *>("/bin/sh"), const_cast<gchar *>("-c"), const_cast<gchar *>(expanded.c_str()), nullptr};
			GError *error = nullptr;
			if (!g_spawn_async(directory.c_str(), argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error))
			{
				showError(std::string("Não foi possível executar o comando.\n\n") + (error ? error->message : ""));
				if (error)
					g_error_free(error);
			}
			else
				setStatusMessage("Executado: " + expanded);
		}
	}
	gtk_widget_destroy(dialog);
}

std::string EditorWindow::documentSummary(EditorDocument &document)
{
	const std::string text = document.text();
	std::size_t characters = 0;
	std::size_t words = 0;
	bool inWord = false;
	for (const char *p = text.c_str(); *p; p = g_utf8_next_char(p))
	{
		const gunichar c = g_utf8_get_char(p);
		if (c != '\r' && c != '\n')
			++characters;
		const bool wordChar = g_unichar_isalnum(c) || c == '_';
		if (wordChar && !inWord)
			++words;
		inWord = wordChar;
	}
	std::ostringstream summary;
	summary << "Caminho completo: " << (document.path.empty() ? "(ainda não salvo)" : document.path.string()) << "\n";
	if (!document.path.empty())
	{
		struct stat info{};
		if (stat(document.path.c_str(), &info) == 0)
		{
			GDateTime *modified = g_date_time_new_from_unix_local(info.st_mtime);
			gchar *formatted = g_date_time_format(modified, "%d/%m/%Y %H:%M:%S");
			summary << "Modificado em: " << (formatted ? formatted : "") << "\n";
			g_free(formatted);
			g_date_time_unref(modified);
		}
	}
	summary << "\nCaracteres (sem fins de linha): " << characters
		<< "\nPalavras: " << words
		<< "\nLinhas: " << document.send(SCI_GETLINECOUNT)
		<< "\nTamanho do documento: " << text.size() << " bytes"
		<< "\nCodificação: " << encodingLabel(document.encoding)
		<< "\nFim de linha: " << eolName(document.eolMode) << "\n";
	const sptr_t selectionStart = document.send(SCI_GETSELECTIONSTART);
	const sptr_t selectionEnd = document.send(SCI_GETSELECTIONEND);
	if (selectionEnd > selectionStart)
	{
		const std::string selected = textRange(document, selectionStart, selectionEnd);
		summary << "\nSeleção: " << g_utf8_strlen(selected.c_str(), static_cast<gssize>(selected.size())) << " caracteres ("
			<< selected.size() << " bytes) em "
			<< document.send(SCI_LINEFROMPOSITION, selectionEnd) - document.send(SCI_LINEFROMPOSITION, selectionStart) + 1 << " linha(s)\n";
	}
	return summary.str();
}

void EditorWindow::showShortcutList()
{
	showTextDialog("Atalhos de teclado", shortcutListText());
}

std::string EditorWindow::shortcutListText() const
{
	std::vector<std::string> rows;
	collectShortcuts(_menuBar, "", rows);
	std::string text = "Atalhos de teclado dos menus (Ctrl+roda do mouse muda o zoom; Alt+Shift+setas seleciona em coluna):\n\n";
	for (const auto &row : rows)
		text += row + "\n";
	text += "\nNesta edição os atalhos são fixos; ainda não é possível trocá-los.\n";
	return text;
}

void EditorWindow::performMore(Action action)
{
	EditorDocument *document = currentDocument();
	GtkWindow *parent = GTK_WINDOW(_window);
	auto needDocument = [&]() { return document != nullptr; };
	auto needPath = [&]() {
		if (!document)
			return false;
		if (document->path.empty())
		{
			setStatusMessage("Salve o documento primeiro.");
			return false;
		}
		return true;
	};
	auto lineOperation = [&](const std::function<bool(std::vector<std::string> &)> &change, bool whole = true) {
		if (document && !transformLines(*document, change, whole))
			setStatusMessage("Nada para alterar.");
	};
	auto caseOperation = [&](CaseKind kind) {
		if (document && !transformSelection(*document, [kind](const std::string &text) { return convertCase(text, kind); }))
			setStatusMessage("Selecione o texto a converter.");
	};
	auto sortOperation = [&](SortKind kind, bool descending) {
		lineOperation([kind, descending](std::vector<std::string> &lines) {
			if (lines.size() < 2)
				return false;
			sortLineList(lines, kind, descending);
			return true;
		});
	};
	auto insertText = [&](const std::string &text) {
		if (document)
			document->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(text.c_str()));
	};
	auto multiSelect = [&](int flags, bool all) {
		if (!document)
			return;
		document->send(SCI_SETSEARCHFLAGS, static_cast<uptr_t>(flags));
		document->send(SCI_TARGETWHOLEDOCUMENT);
		if (document->send(SCI_GETSELECTIONEMPTY))
		{
			// The first call selects the word at the caret.
			document->send(all ? SCI_MULTIPLESELECTADDEACH : SCI_MULTIPLESELECTADDNEXT);
			if (!all)
				return;
		}
		document->send(all ? SCI_MULTIPLESELECTADDEACH : SCI_MULTIPLESELECTADDNEXT);
		setStatusMessage(std::to_string(document->send(SCI_GETSELECTIONS)) + " seleção(ões).");
	};
	auto findWith = [&](bool backwards, bool fromSelection) {
		if (!document)
			return;
		// After the Find dialog, F3 repeats its search with the same options.
		if (_dialogSearchActive && !fromSelection && _findWindow)
		{
			runFindOperation(FindOperation::Next, backwards ? 1 : 0);
			return;
		}
		std::string needle = gtk_entry_get_text(GTK_ENTRY(_searchEntry));
		if (fromSelection || needle.empty())
		{
			needle = currentWordOrSelection(*document);
			if (needle.empty() || needle.find_first_of("\r\n") != std::string::npos)
			{
				showFindDialog(0);
				return;
			}
			gtk_entry_set_text(GTK_ENTRY(_searchEntry), needle.c_str());
		}
		if (fromSelection)
		{
			// Start after (or before) the selection so the next match is found.
			const sptr_t from = backwards ? document->send(SCI_GETSELECTIONSTART) : document->send(SCI_GETSELECTIONEND);
			document->send(SCI_SETSEL, from, from);
		}
		showFindResults(backwards ? "back" : "next");
	};
	auto copyNames = [&](bool fullPath) {
		std::string text;
		for (const auto &owned : _documents)
			text += (fullPath && !owned->path.empty() ? owned->path.string() : owned->displayName) + "\n";
		copyToClipboard(text);
		setStatusMessage("Copiado para a área de transferência.");
	};
	auto openFolder = [&](const std::filesystem::path &folder) {
		std::string error;
		if (!launchUri(fileUri(folder), error))
			showError("Não foi possível abrir a pasta.\n\n" + error);
	};
	auto bookmarkedLines = [&](bool marked) {
		std::vector<sptr_t> lines;
		const sptr_t count = document->send(SCI_GETLINECOUNT);
		for (sptr_t line = 0; line < count; ++line)
		{
			if (((document->send(SCI_MARKERGET, line) & BookmarkMask) != 0) == marked)
				lines.push_back(line);
		}
		// The empty line after a final line break is never removed.
		if (!marked && !lines.empty() && lines.back() == count - 1 && document->send(SCI_POSITIONFROMLINE, count - 1) == document->send(SCI_GETLENGTH))
			lines.pop_back();
		return lines;
	};
	auto removeLines = [&](const std::vector<sptr_t> &lines) {
		// Bookmarks of removed lines would otherwise move to the neighbouring line.
		for (const sptr_t line : lines)
			document->send(SCI_MARKERDELETE, line, BookmarkMarker);
		document->send(SCI_BEGINUNDOACTION);
		for (auto it = lines.rbegin(); it != lines.rend(); ++it)
		{
			const sptr_t start = document->send(SCI_POSITIONFROMLINE, *it);
			sptr_t end = document->send(SCI_POSITIONFROMLINE, *it + 1);
			sptr_t from = start;
			if (*it + 1 >= document->send(SCI_GETLINECOUNT))
			{
				// Last line: remove the line break before it instead.
				end = document->send(SCI_GETLENGTH);
				from = *it > 0 ? document->send(SCI_GETLINEENDPOSITION, *it - 1) : start;
			}
			document->send(SCI_DELETERANGE, from, end - from);
		}
		document->send(SCI_ENDUNDOACTION);
	};
	auto copyLines = [&](const std::vector<sptr_t> &lines) {
		std::string text;
		for (const sptr_t line : lines)
		{
			std::string current = lineText(*document, line, true);
			if (current.empty() || (current.back() != '\n' && current.back() != '\r'))
				current += eolString(*document);
			text += current;
		}
		copyToClipboard(text);
	};
	auto jumpChange = [&](bool forward) {
		const sptr_t count = document->send(SCI_GETLINECOUNT);
		const sptr_t line = currentLine(*document);
		auto changed = [&](sptr_t l) { return l >= 0 && l < count && (document->send(SCI_MARKERGET, l) & HistoryMask) != 0; };
		auto blockStart = [&](sptr_t l) { return changed(l) && !changed(l - 1); };
		for (sptr_t step = 1; step <= count; ++step)
		{
			const sptr_t candidate = ((forward ? line + step : line - step) % count + count) % count;
			if (blockStart(candidate))
			{
				document->send(SCI_ENSUREVISIBLEENFORCEPOLICY, candidate);
				document->send(SCI_GOTOLINE, candidate);
				return;
			}
		}
		setStatusMessage("Não há alterações neste documento.");
	};
	auto jumpStyle = [&](bool forward) {
		const auto ranges = tokenRanges(*document);
		if (ranges.empty())
		{
			setStatusMessage("Não há destaques neste documento.");
			return;
		}
		const sptr_t caret = forward ? document->send(SCI_GETSELECTIONEND) : document->send(SCI_GETSELECTIONSTART);
		const IndicatorRange *target = nullptr;
		if (forward)
		{
			for (const auto &range : ranges)
			{
				if (range.start >= caret)
				{
					target = &range;
					break;
				}
			}
			if (!target)
				target = &ranges.front();
		}
		else
		{
			for (auto it = ranges.rbegin(); it != ranges.rend(); ++it)
			{
				if (it->end <= caret)
				{
					target = &*it;
					break;
				}
			}
			if (!target)
				target = &ranges.back();
		}
		document->send(SCI_SETSEL, target->start, target->end);
		document->send(SCI_SCROLLCARET);
	};

	switch (action)
	{
		// Arquivo
		case Action::OpenContainingFolder:
			if (needPath())
				openFolder(document->path.parent_path());
			break;
		case Action::OpenContainingTerminal:
		{
			const std::filesystem::path folder = document && !document->path.empty() ? document->path.parent_path() : std::filesystem::path(g_get_home_dir());
			bool started = false;
			for (const char *terminal : {"x-terminal-emulator", "gnome-terminal", "mate-terminal", "xfce4-terminal", "konsole", "xterm"})
			{
				gchar *program = g_find_program_in_path(terminal);
				if (!program)
					continue;
				gchar *argv[] = {program, nullptr};
				GError *error = nullptr;
				started = g_spawn_async(folder.c_str(), argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error);
				if (error)
					g_error_free(error);
				g_free(program);
				if (started)
					break;
			}
			if (!started)
				showError("Nenhum terminal foi encontrado neste sistema.");
			break;
		}
		case Action::OpenFolderAsWorkspace:
			if (needPath())
			{
				addWorkspaceFolder(document->path.parent_path());
				_settings.showWorkspace = true;
				applySettings();
				saveSettings();
			}
			break;
		case Action::OpenInDefaultViewer:
			if (needPath())
			{
				std::string error;
				if (!launchUri(fileUri(document->path), error))
					showError("Não foi possível abrir no visualizador padrão.\n\n" + error);
			}
			break;
		case Action::ReloadFromDisk:
			if (needPath())
				reloadFromDisk(*document, true);
			break;
		case Action::SaveCopyAs:
			if (needDocument() && !_selfTesting)
			{
				const auto target = chooseFile(parent, "Salvar uma cópia como", GTK_FILE_CHOOSER_ACTION_SAVE, "_Salvar",
					document->path.empty() ? std::filesystem::path() : document->path.parent_path(), document->displayName);
				std::string error;
				if (!target.empty() && !document->writeCopy(target, error))
					showError("Não foi possível salvar a cópia.\n\n" + error);
				else if (!target.empty())
					setStatusMessage("Cópia salva em " + target.string());
			}
			break;
		case Action::SaveAll:
			for (std::size_t i = 0; i < _documents.size(); ++i)
			{
				if (_documents[i]->send(SCI_GETMODIFY))
				{
					setCurrentPage(static_cast<int>(i));
					if (!saveDocument(*_documents[i]))
						break;
				}
			}
			break;
		case Action::RenameFile:
		{
			if (!document || _selfTesting)
				break;
			const std::string name = askText(parent, "Renomear", document->path.empty() ? "Novo nome da aba:" : "Novo nome do arquivo:",
				document->path.empty() ? document->displayName : document->path.filename().string());
			if (!name.empty() && name.find('/') == std::string::npos)
				renameDocument(*document, name);
			break;
		}
		case Action::CloseAll:
		case Action::CloseAllButActive:
		case Action::CloseAllToLeft:
		case Action::CloseAllToRight:
		case Action::CloseAllUnchanged:
			closeMultiple(action);
			return;
		case Action::MoveToTrash:
		{
			if (!needPath() || _selfTesting)
				break;
			if (!confirm(parent, "Mover “" + document->displayName + "” para a lixeira?", "O arquivo pode ser recuperado na lixeira do sistema.", "_Mover para a lixeira"))
				break;
			GFile *file = g_file_new_for_path(document->path.c_str());
			GError *error = nullptr;
			const bool trashed = g_file_trash(file, nullptr, &error);
			if (!trashed)
				showError(std::string("Não foi possível mover para a lixeira.\n\n") + (error ? error->message : ""));
			if (error)
				g_error_free(error);
			g_object_unref(file);
			if (trashed)
			{
				closeDocument(currentIndex(), false);
				return;
			}
			break;
		}
		case Action::LoadSessionFile:
			if (!_selfTesting)
			{
				const auto file = chooseFile(parent, "Carregar sessão", GTK_FILE_CHOOSER_ACTION_OPEN, "_Abrir");
				if (!file.empty() && !openSessionFile(file))
					showError("O arquivo escolhido não é uma sessão do Notepad++.");
			}
			return;
		case Action::SaveSessionFile:
			if (!_selfTesting)
			{
				const auto file = chooseFile(parent, "Salvar sessão", GTK_FILE_CHOOSER_ACTION_SAVE, "_Salvar", {}, "sessao.xml");
				if (!file.empty() && !saveSessionFileAs(file))
					showError("Não foi possível salvar a sessão.");
			}
			break;
		case Action::Print:
			printDocument(true);
			break;
		case Action::PrintNow:
			printDocument(false);
			break;

		// Editar
		case Action::InsertDateShort:
			insertText(formatNow("%H:%M %x"));
			break;
		case Action::InsertDateLong:
			insertText(formatNow("%H:%M %A, %d de %B de %Y"));
			break;
		case Action::InsertDateCustom:
			insertText(formatNow(_settings.dateTimeFormat));
			break;
		case Action::CopyFullPath:
			if (document)
			{
				copyToClipboard(document->path.empty() ? document->displayName : document->path.string());
				setStatusMessage("Caminho copiado.");
			}
			break;
		case Action::CopyFileName:
			if (document)
			{
				copyToClipboard(document->path.empty() ? document->displayName : document->path.filename().string());
				setStatusMessage("Nome copiado.");
			}
			break;
		case Action::CopyDirectoryPath:
			if (needPath())
			{
				copyToClipboard(document->path.parent_path().string());
				setStatusMessage("Pasta copiada.");
			}
			break;
		case Action::CopyAllNames:
			copyNames(false);
			break;
		case Action::CopyAllPaths:
			copyNames(true);
			break;
		case Action::IndentIncrease:
			if (document) document->send(SCI_TAB);
			break;
		case Action::IndentDecrease:
			if (document) document->send(SCI_BACKTAB);
			break;
		case Action::ProperCase: caseOperation(CaseKind::Proper); break;
		case Action::ProperCaseBlend: caseOperation(CaseKind::ProperBlend); break;
		case Action::SentenceCase: caseOperation(CaseKind::Sentence); break;
		case Action::SentenceCaseBlend: caseOperation(CaseKind::SentenceBlend); break;
		case Action::InvertCase: caseOperation(CaseKind::Invert); break;
		case Action::RandomCase: caseOperation(CaseKind::Random); break;
		case Action::RemoveConsecutiveDuplicates:
			lineOperation([](std::vector<std::string> &lines) { return removeConsecutiveDuplicates(lines) > 0; });
			break;
		case Action::RemoveEmptyLines:
			lineOperation([](std::vector<std::string> &lines) { return removeEmptyLines(lines, false) > 0; });
			break;
		case Action::RemoveBlankLines:
			lineOperation([](std::vector<std::string> &lines) { return removeEmptyLines(lines, true) > 0; });
			break;
		case Action::ReverseLines:
			lineOperation([](std::vector<std::string> &lines) { std::reverse(lines.begin(), lines.end()); return lines.size() > 1; });
			break;
		case Action::RandomizeLines:
			lineOperation([](std::vector<std::string> &lines) { shuffleLines(lines); return lines.size() > 1; });
			break;
		case Action::SplitLines:
		case Action::JoinLines:
			if (document)
			{
				sptr_t first = 0;
				sptr_t last = 0;
				selectedLines(*document, first, last);
				if (action == Action::JoinLines && first == last)
				{
					setStatusMessage("Selecione as linhas a juntar.");
					break;
				}
				document->send(SCI_SETTARGETRANGE, document->send(SCI_POSITIONFROMLINE, first), document->send(SCI_GETLINEENDPOSITION, last));
				document->send(SCI_BEGINUNDOACTION);
				if (action == Action::JoinLines)
					document->send(SCI_LINESJOIN);
				else
				{
					const int edge = _settings.edgeColumn > 0 ? _settings.edgeColumn : 0;
					const std::string sample(static_cast<std::size_t>(edge), 'x');
					const int width = edge > 0 ? static_cast<int>(document->send(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<sptr_t>(sample.c_str()))) : 0;
					document->send(SCI_LINESSPLIT, static_cast<uptr_t>(width));
				}
				document->send(SCI_ENDUNDOACTION);
			}
			break;
		case Action::InsertLineAbove:
		case Action::InsertLineBelow:
			if (document)
			{
				const sptr_t line = currentLine(*document);
				const std::string eol = eolString(*document);
				const sptr_t position = action == Action::InsertLineAbove ? document->send(SCI_POSITIONFROMLINE, line) : document->send(SCI_GETLINEENDPOSITION, line);
				document->send(SCI_INSERTTEXT, position, reinterpret_cast<sptr_t>(eol.c_str()));
				document->send(SCI_GOTOPOS, action == Action::InsertLineAbove ? position : position + static_cast<sptr_t>(eol.size()));
			}
			break;
		case Action::SortLexicalAsc: sortOperation(SortKind::Lexical, false); break;
		case Action::SortIgnoreCaseAsc: sortOperation(SortKind::LexicalIgnoreCase, false); break;
		case Action::SortLocaleAsc: sortOperation(SortKind::Locale, false); break;
		case Action::SortIntegerAsc: sortOperation(SortKind::Integer, false); break;
		case Action::SortDecimalCommaAsc: sortOperation(SortKind::DecimalComma, false); break;
		case Action::SortDecimalDotAsc: sortOperation(SortKind::DecimalDot, false); break;
		case Action::SortLengthAsc: sortOperation(SortKind::Length, false); break;
		case Action::SortLexicalDesc: sortOperation(SortKind::Lexical, true); break;
		case Action::SortIgnoreCaseDesc: sortOperation(SortKind::LexicalIgnoreCase, true); break;
		case Action::SortLocaleDesc: sortOperation(SortKind::Locale, true); break;
		case Action::SortIntegerDesc: sortOperation(SortKind::Integer, true); break;
		case Action::SortDecimalCommaDesc: sortOperation(SortKind::DecimalComma, true); break;
		case Action::SortDecimalDotDesc: sortOperation(SortKind::DecimalDot, true); break;
		case Action::SortLengthDesc: sortOperation(SortKind::Length, true); break;
		case Action::LineComment:
		case Action::LineUncomment:
			if (document && !setLineComment(*document, commentPrefix(*document), action == Action::LineComment))
				setStatusMessage("Esta linguagem não tem comentário de linha.");
			break;
		case Action::BlockComment:
		case Action::BlockUncomment:
			if (document)
			{
				std::pair<std::string, std::string> delimiters = blockCommentFor(document->language->id);
				if (const UserLanguage *user = userLanguageFor(document->language))
					delimiters = {user->blockOpen, user->blockClose};
				if (delimiters.first.empty())
					setStatusMessage("Esta linguagem não tem comentário em bloco.");
				else if (!setBlockComment(*document, delimiters.first, delimiters.second, action == Action::BlockComment))
					setStatusMessage("Nenhum comentário em bloco encontrado no cursor.");
			}
			break;
		case Action::WordCompletion:
			if (document)
			{
				const auto words = completionWords(*document);
				if (words.empty())
				{
					setStatusMessage("Nenhuma palavra do documento completa o que foi digitado.");
					break;
				}
				const sptr_t position = document->send(SCI_GETCURRENTPOS);
				const sptr_t start = document->send(SCI_WORDSTARTPOSITION, position, 1);
				std::string list;
				for (const auto &entry : words)
					list += (list.empty() ? "" : " ") + entry;
				document->send(SCI_AUTOCSETSEPARATOR, ' ');
				document->send(SCI_AUTOCSETIGNORECASE, 1);
				document->send(SCI_AUTOCSHOW, static_cast<uptr_t>(position - start), reinterpret_cast<sptr_t>(list.c_str()));
			}
			break;
		case Action::PathCompletion:
			if (document)
			{
				const sptr_t position = document->send(SCI_GETCURRENTPOS);
				const sptr_t lineStart = document->send(SCI_POSITIONFROMLINE, currentLine(*document));
				const std::string before = textRange(*document, lineStart, position);
				const auto cut = before.find_last_of(" \t\"'<>=(");
				std::string expanded = cut == std::string::npos ? before : before.substr(cut + 1);
				if (!expanded.empty() && expanded[0] == '~')
					expanded = std::string(g_get_home_dir()) + expanded.substr(1);
				std::filesystem::path folder;
				std::string partial;
				const auto slash = expanded.rfind('/');
				if (slash == std::string::npos)
				{
					folder = document->path.empty() ? std::filesystem::path(g_get_home_dir()) : document->path.parent_path();
					partial = expanded;
				}
				else
				{
					folder = expanded.substr(0, slash + 1);
					if (folder.is_relative() && !document->path.empty())
						folder = document->path.parent_path() / folder;
					partial = expanded.substr(slash + 1);
				}
				std::vector<std::string> names;
				std::error_code error;
				for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end && names.size() < 500; it.increment(error))
				{
					const std::string name = it->path().filename().string();
					std::error_code typeError;
					if (name.rfind(partial, 0) == 0 && (!partial.empty() || name[0] != '.'))
						names.push_back(name + (it->is_directory(typeError) ? "/" : ""));
				}
				std::sort(names.begin(), names.end());
				if (names.empty())
				{
					setStatusMessage("Nenhum arquivo ou pasta corresponde.");
					break;
				}
				std::string list;
				for (const auto &name : names)
					list += (list.empty() ? "" : "\n") + name;
				document->send(SCI_AUTOCSETSEPARATOR, '\n');
				document->send(SCI_AUTOCSETIGNORECASE, 0);
				document->send(SCI_AUTOCSHOW, partial.size(), reinterpret_cast<sptr_t>(list.c_str()));
			}
			break;
		case Action::ConvertToMacEol:
			if (document)
			{
				document->send(SCI_SETEOLMODE, SC_EOL_CR);
				document->send(SCI_CONVERTEOLS, SC_EOL_CR);
				document->eolMode = EolMode::MacClassic;
			}
			break;
		case Action::TrimLeading:
			lineOperation([](std::vector<std::string> &lines) { for (auto &line : lines) line = trimLeading(line); return true; });
			break;
		case Action::TrimBoth:
			lineOperation([](std::vector<std::string> &lines) { for (auto &line : lines) line = trimTrailing(trimLeading(line)); return true; });
			break;
		case Action::EolToSpace:
		case Action::TrimBothEolToSpace:
		{
			const bool trim = action == Action::TrimBothEolToSpace;
			lineOperation([trim](std::vector<std::string> &lines) {
				if (lines.size() < 2 && !trim)
					return false;
				std::string joined;
				bool first = true;
				for (const auto &line : lines)
				{
					const std::string part = trim ? trimTrailing(trimLeading(line)) : line;
					if (trim && part.empty())
						continue;
					joined += (first ? "" : " ") + part;
					first = false;
				}
				lines = {joined};
				return true;
			});
			break;
		}
		case Action::TabsToSpaces:
		{
			const int width = _settings.tabWidth;
			lineOperation([width](std::vector<std::string> &lines) { for (auto &line : lines) line = tabsToSpaces(line, width); return true; });
			break;
		}
		case Action::SpacesToTabsAll:
		case Action::SpacesToTabsLeading:
		{
			const int width = _settings.tabWidth;
			const bool leading = action == Action::SpacesToTabsLeading;
			lineOperation([width, leading](std::vector<std::string> &lines) { for (auto &line : lines) line = spacesToTabs(line, width, leading); return true; });
			break;
		}
		case Action::OpenSelectedFile:
		case Action::OpenSelectedFolder:
			if (document)
			{
				std::string text = trimTrailing(trimLeading(currentWordOrSelection(*document)));
				if (text.empty())
				{
					setStatusMessage("Selecione o caminho de um arquivo.");
					break;
				}
				if (text[0] == '~')
					text = std::string(g_get_home_dir()) + text.substr(1);
				std::filesystem::path path = text;
				if (path.is_relative() && !document->path.empty())
					path = document->path.parent_path() / path;
				std::error_code error;
				if (action == Action::OpenSelectedFile && std::filesystem::is_regular_file(path, error))
				{
					openPath(path);
					return;
				}
				if (action == Action::OpenSelectedFolder && std::filesystem::exists(path, error))
					openFolder(std::filesystem::is_directory(path, error) ? path : path.parent_path());
				else
					setStatusMessage("Não encontrado: " + path.string());
			}
			break;
		case Action::SearchOnInternet:
			if (document)
			{
				const std::string text = currentWordOrSelection(*document);
				if (text.empty())
				{
					setStatusMessage("Selecione o texto a pesquisar.");
					break;
				}
				gchar *escaped = g_uri_escape_string(text.c_str(), nullptr, FALSE);
				const std::string query = escaped ? escaped : "";
				g_free(escaped);
				const std::string url = expandVariables(_settings.searchEngine, {{"TEXT", query}, {"CURRENT_WORD", query}});
				std::string error;
				if (!_selfTesting && !launchUri(url, error))
					showError("Não foi possível abrir o navegador.\n\n" + error);
			}
			break;
		case Action::SelectAllMatches: multiSelect(0, true); break;
		case Action::SelectAllMatchesCase: multiSelect(SCFIND_MATCHCASE, true); break;
		case Action::SelectAllMatchesWord: multiSelect(SCFIND_WHOLEWORD, true); break;
		case Action::SelectAllMatchesCaseWord: multiSelect(SCFIND_MATCHCASE | SCFIND_WHOLEWORD, true); break;
		case Action::SelectNextMatch: multiSelect(0, false); break;
		case Action::SelectNextMatchCase: multiSelect(SCFIND_MATCHCASE, false); break;
		case Action::SelectNextMatchWord: multiSelect(SCFIND_WHOLEWORD, false); break;
		case Action::SelectNextMatchCaseWord: multiSelect(SCFIND_MATCHCASE | SCFIND_WHOLEWORD, false); break;
		case Action::UndoLatestMultiSelect:
			if (document && document->send(SCI_GETSELECTIONS) > 1)
				document->send(SCI_DROPSELECTIONN, static_cast<uptr_t>(document->send(SCI_GETMAINSELECTION)));
			break;
		case Action::SkipMultiSelect:
			if (document && document->send(SCI_GETSELECTIONS) > 1)
			{
				const sptr_t main = document->send(SCI_GETMAINSELECTION);
				document->send(SCI_TARGETWHOLEDOCUMENT);
				document->send(SCI_MULTIPLESELECTADDNEXT);
				document->send(SCI_DROPSELECTIONN, static_cast<uptr_t>(main));
			}
			break;
		case Action::ColumnModeInfo:
			showMessage("Modo coluna\n\n• Teclado: segure Alt+Shift e use as setas.\n• Mouse: segure Alt e arraste. Se o sistema usar Alt+arrastar para mover janelas, use o teclado.\n\nDepois de selecionar em coluna, o que você digitar entra em todas as linhas. Para numerar linhas, use Editar → Editor de colunas (Alt+C).");
			break;
		case Action::ColumnEditor:
			showColumnEditor();
			break;
		case Action::ToggleReadOnly:
			if (document)
			{
				document->send(SCI_SETREADONLY, document->send(SCI_GETREADONLY) ? 0 : 1);
				setStatusMessage(document->send(SCI_GETREADONLY) ? "Documento somente leitura." : "Documento liberado para edição.");
			}
			break;
		case Action::ReadOnlyAll:
		case Action::ClearReadOnlyAll:
			for (const auto &owned : _documents)
			{
				if (!owned->monitoring)
					owned->send(SCI_SETREADONLY, action == Action::ReadOnlyAll ? 1 : 0);
			}
			break;

		// Pesquisar
		case Action::FindNext: findWith(false, false); break;
		case Action::FindPrevious: findWith(true, false); break;
		case Action::SelectFindNext: findWith(false, true); break;
		case Action::SelectFindPrevious: findWith(true, true); break;
		case Action::GoToMatchingBrace:
			if (document)
			{
				const sptr_t position = document->send(SCI_GETCURRENTPOS);
				sptr_t match = document->send(SCI_BRACEMATCH, position, 0);
				if (match < 0 && position > 0)
					match = document->send(SCI_BRACEMATCH, position - 1, 0);
				if (match >= 0)
					document->send(SCI_GOTOPOS, match);
				else
					setStatusMessage("Nenhum ( ) [ ] { } no cursor.");
			}
			break;
		case Action::SelectBetweenBraces:
			if (document)
			{
				const sptr_t position = document->send(SCI_GETCURRENTPOS);
				const std::string before = textRange(*document, 0, position);
				bool found = false;
				for (sptr_t p = position - 1; p >= 0 && !found; --p)
				{
					const char c = before[static_cast<std::size_t>(p)];
					if (c != '(' && c != '[' && c != '{')
						continue;
					const sptr_t match = document->send(SCI_BRACEMATCH, p, 0);
					if (match >= position)
					{
						document->send(SCI_SETSEL, p + 1, match);
						found = true;
					}
				}
				if (!found)
					setStatusMessage("O cursor não está entre ( ) [ ] { }.");
			}
			break;
		case Action::StyleToken1: case Action::StyleToken2: case Action::StyleToken3: case Action::StyleToken4: case Action::StyleToken5:
			if (document)
				styleAllOccurrences(*document, static_cast<int>(action) - static_cast<int>(Action::StyleToken1));
			break;
		case Action::ClearStyle1: case Action::ClearStyle2: case Action::ClearStyle3: case Action::ClearStyle4: case Action::ClearStyle5:
		case Action::ClearAllStyles:
			if (document)
			{
				const int first = action == Action::ClearAllStyles ? 0 : static_cast<int>(action) - static_cast<int>(Action::ClearStyle1);
				const int last = action == Action::ClearAllStyles ? TokenStyles - 1 : first;
				for (int style = first; style <= last; ++style)
				{
					document->send(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(FirstTokenIndicator + style));
					document->send(SCI_INDICATORCLEARRANGE, 0, document->send(SCI_GETLENGTH));
				}
			}
			break;
		case Action::JumpStyleUp: if (document) jumpStyle(false); break;
		case Action::JumpStyleDown: if (document) jumpStyle(true); break;
		case Action::ToggleBookmark:
			if (document)
				toggleBookmark(*document, currentLine(*document));
			break;
		case Action::NextBookmark:
		case Action::PreviousBookmark:
			if (document)
			{
				const sptr_t line = currentLine(*document);
				const bool next = action == Action::NextBookmark;
				sptr_t target = next ? document->send(SCI_MARKERNEXT, line + 1, BookmarkMask) : document->send(SCI_MARKERPREVIOUS, line - 1, BookmarkMask);
				if (target < 0)
					target = next ? document->send(SCI_MARKERNEXT, 0, BookmarkMask) : document->send(SCI_MARKERPREVIOUS, document->send(SCI_GETLINECOUNT), BookmarkMask);
				if (target >= 0)
				{
					document->send(SCI_ENSUREVISIBLEENFORCEPOLICY, target);
					document->send(SCI_GOTOLINE, target);
				}
				else
					setStatusMessage("Não há marcadores. Use Ctrl+F2 ou clique na margem para marcar uma linha.");
			}
			break;
		case Action::ClearBookmarks:
			if (document) document->send(SCI_MARKERDELETEALL, BookmarkMarker);
			break;
		case Action::CopyBookmarkedLines:
			if (document) copyLines(bookmarkedLines(true));
			break;
		case Action::CutBookmarkedLines:
			if (document)
			{
				const auto lines = bookmarkedLines(true);
				copyLines(lines);
				removeLines(lines);
			}
			break;
		case Action::RemoveBookmarkedLines:
			if (document) removeLines(bookmarkedLines(true));
			break;
		case Action::RemoveUnbookmarkedLines:
			if (document) removeLines(bookmarkedLines(false));
			break;
		case Action::InverseBookmarks:
			if (document)
			{
				const sptr_t count = document->send(SCI_GETLINECOUNT);
				for (sptr_t line = 0; line < count; ++line)
					toggleBookmark(*document, line);
			}
			break;
		case Action::NextChange: if (document) jumpChange(true); break;
		case Action::PreviousChange: if (document) jumpChange(false); break;

		// Exibir
		case Action::AlwaysOnTop:
			gtk_window_set_keep_above(parent, gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(_alwaysOnTopCheck)));
			break;
		case Action::FullScreen:
			_fullScreen = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(_fullScreenCheck));
			if (_fullScreen || _distractionFree)
				gtk_window_fullscreen(parent);
			else
				gtk_window_unfullscreen(parent);
			break;
		case Action::DistractionFree:
			_distractionFree = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(_distractionFreeCheck));
			gtk_widget_set_visible(_toolbar, !_distractionFree);
			gtk_widget_set_visible(_statusBar, !_distractionFree);
			gtk_notebook_set_show_tabs(GTK_NOTEBOOK(_notebook), !_distractionFree);
			if (_distractionFree)
			{
				gtk_widget_hide(_workspacePanel);
				gtk_widget_hide(_sidebar);
				gtk_window_fullscreen(parent);
			}
			else
			{
				applyPanels();
				if (!_fullScreen)
					gtk_window_unfullscreen(parent);
			}
			for (const auto &owned : _documents)
				owned->send(SCI_SETMARGINWIDTHN, 0, _distractionFree ? 0 : (_settings.lineNumbers ? 52 : 0));
			break;
		case Action::ShowAllCharacters:
		{
			const bool all = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(_allCharactersCheck));
			_settings.showWhitespace = all;
			_settings.showEndOfLine = all;
			applySettings();
			saveSettings();
			break;
		}
		case Action::ToggleIndentGuides:
			_settings.indentGuides = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(_indentGuidesCheck));
			applySettings();
			saveSettings();
			break;
		case Action::ToggleWrapSymbol:
			_settings.showWrapSymbol = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(_wrapSymbolCheck));
			applySettings();
			saveSettings();
			break;
		case Action::Tab1: case Action::Tab2: case Action::Tab3: case Action::Tab4: case Action::Tab5:
		case Action::Tab6: case Action::Tab7: case Action::Tab8: case Action::Tab9:
			goToPage(static_cast<int>(action) - static_cast<int>(Action::Tab1));
			return;
		case Action::FirstTab:
			goToPage(0);
			return;
		case Action::LastTab:
			goToPage(static_cast<int>(_documents.size()) - 1);
			return;
		case Action::NextTab:
		case Action::PreviousTab:
			if (!_documents.empty())
			{
				const int count = static_cast<int>(_documents.size());
				const int current = static_cast<int>(currentIndex());
				goToPage(((action == Action::NextTab ? current + 1 : current - 1) % count + count) % count);
			}
			return;
		case Action::MoveTabStart:
			moveTab(currentIndex(), 0);
			return;
		case Action::MoveTabEnd:
			if (!_documents.empty())
				moveTab(currentIndex(), _documents.size() - 1);
			return;
		case Action::MoveTabForward:
			moveTab(currentIndex(), currentIndex() + 1);
			return;
		case Action::MoveTabBackward:
			if (currentIndex() > 0)
				moveTab(currentIndex(), currentIndex() - 1);
			return;
		case Action::FoldAll:
		case Action::UnfoldAll:
			if (document)
			{
				document->send(SCI_COLOURISE, 0, -1);
				document->send(SCI_FOLDALL, action == Action::FoldAll ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
			}
			break;
		case Action::FoldCurrent:
		case Action::UnfoldCurrent:
			if (document)
			{
				document->send(SCI_COLOURISE, 0, -1);
				const sptr_t line = currentLine(*document);
				const sptr_t header = (document->send(SCI_GETFOLDLEVEL, line) & SC_FOLDLEVELHEADERFLAG) ? line : document->send(SCI_GETFOLDPARENT, line);
				if (header >= 0)
					document->send(SCI_FOLDLINE, header, action == Action::FoldCurrent ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
			}
			break;
		case Action::FoldLevel1: case Action::FoldLevel2: case Action::FoldLevel3: case Action::FoldLevel4:
		case Action::FoldLevel5: case Action::FoldLevel6: case Action::FoldLevel7: case Action::FoldLevel8:
			if (document)
				foldLevel(*document, static_cast<int>(action) - static_cast<int>(Action::FoldLevel1) + 1, true);
			break;
		case Action::UnfoldLevel1: case Action::UnfoldLevel2: case Action::UnfoldLevel3: case Action::UnfoldLevel4:
		case Action::UnfoldLevel5: case Action::UnfoldLevel6: case Action::UnfoldLevel7: case Action::UnfoldLevel8:
			if (document)
				foldLevel(*document, static_cast<int>(action) - static_cast<int>(Action::UnfoldLevel1) + 1, false);
			break;
		case Action::HideLines:
			if (document)
			{
				sptr_t first = 0;
				sptr_t last = 0;
				selectedLines(*document, first, last);
				if (first == 0 && last >= document->send(SCI_GETLINECOUNT) - 1)
				{
					setStatusMessage("Não é possível ocultar todas as linhas.");
					break;
				}
				document->send(SCI_HIDELINES, first, last);
				setStatusMessage("Linhas ocultas. Use Exibir → Mostrar linhas ocultas para vê-las de novo.");
			}
			break;
		case Action::ShowHiddenLines:
			if (document) document->send(SCI_SHOWLINES, 0, document->send(SCI_GETLINECOUNT) - 1);
			break;
		case Action::Summary:
			if (document)
				showTextDialog("Resumo", documentSummary(*document));
			break;
		case Action::Monitoring:
			if (document)
			{
				const bool enable = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(_monitoringCheck));
				if (enable && (document->path.empty() || document->send(SCI_GETMODIFY)))
				{
					setStatusMessage("Salve o documento antes de monitorá-lo.");
					_updating = true;
					gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_monitoringCheck), FALSE);
					_updating = false;
					break;
				}
				document->monitoring = enable;
				document->send(SCI_SETREADONLY, enable ? 1 : 0);
				if (enable)
				{
					document->monitoredSize = static_cast<std::uintmax_t>(-1);
					monitorTick();
					setStatusMessage("Monitorando o arquivo: novas linhas aparecem sozinhas (somente leitura).");
				}
			}
			break;

		// Configurações
		case Action::ShortcutList:
			showShortcutList();
			break;
		case Action::ImportPlugin:
			if (!_selfTesting)
			{
				const auto file = chooseFile(parent, "Importar plugin (.so)", GTK_FILE_CHOOSER_ACTION_OPEN, "_Importar");
				if (file.empty())
					break;
				if (file.extension() != ".so")
				{
					showError("Escolha um plugin para Linux (arquivo .so). Plugins DLL do Windows não funcionam.");
					break;
				}
				const auto folder = PluginManager::defaultDirectories().front() / file.stem();
				std::error_code error;
				std::filesystem::create_directories(folder, error);
				std::filesystem::copy_file(file, folder / file.filename(), std::filesystem::copy_options::overwrite_existing, error);
				if (error)
				{
					showError("Não foi possível copiar o plugin.\n\n" + error.message());
					break;
				}
				std::filesystem::permissions(folder / file.filename(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
					| std::filesystem::perms::group_read | std::filesystem::perms::others_read, error);
				scanPlugins(PluginManager::defaultDirectories());
				showMessage("Plugin copiado. Ele continua desativado até você ativá-lo em Plugins → Gerenciar plugins.");
			}
			break;
		case Action::OpenUserLanguageFolder:
		{
			std::error_code error;
			std::filesystem::create_directories(userLanguageDirectory(), error);
			if (!_selfTesting)
				openFolder(userLanguageDirectory());
			break;
		}

		// Ferramentas
		case Action::HashMd5Text: case Action::HashMd5Files: case Action::HashMd5Selection:
		case Action::HashSha1Text: case Action::HashSha1Files: case Action::HashSha1Selection:
		case Action::HashSha256Text: case Action::HashSha256Files: case Action::HashSha256Selection:
		case Action::HashSha512Text: case Action::HashSha512Files: case Action::HashSha512Selection:
		{
			const int offset = static_cast<int>(action) - static_cast<int>(Action::HashMd5Text);
			const int kind = offset / 3;
			const int mode = offset % 3;
			if (mode == 2)
			{
				if (!document)
					break;
				const sptr_t start = document->send(SCI_GETSELECTIONSTART);
				const sptr_t end = document->send(SCI_GETSELECTIONEND);
				if (end <= start)
				{
					setStatusMessage("Selecione o texto primeiro.");
					break;
				}
				copyToClipboard(hashHex(static_cast<HashKind>(kind), textRange(*document, start, end)));
				setStatusMessage(std::string(hashName(static_cast<HashKind>(kind))) + " da seleção copiado para a área de transferência.");
			}
			else
				showHashDialog(kind, mode == 1);
			break;
		}

		// Macro
		case Action::SaveMacroAs:
		{
			if (_macro.empty())
			{
				setStatusMessage("Grave uma macro primeiro (Macro → Iniciar gravação).");
				break;
			}
			if (_selfTesting)
				break;
			const std::string name = sanitizeName(askText(parent, "Salvar macro", "Nome da macro (aparece no menu Macro):", ""));
			if (name.empty())
				break;
			const auto file = _sessionStore.macroDirectory() / (name + ".xml");
			std::error_code error;
			if (std::filesystem::exists(file, error) && !confirm(parent, "Substituir a macro “" + name + "”?", "", "_Substituir"))
				break;
			if (!SessionStore::saveMacroFile(file, _macro))
				showError("Não foi possível salvar a macro.");
			rebuildMacroMenu();
			break;
		}
		case Action::RunMacroMultiple:
		{
			if (_selfTesting || !document)
				break;
			std::vector<std::filesystem::path> files;
			std::error_code error;
			for (std::filesystem::directory_iterator it(_sessionStore.macroDirectory(), error), end; !error && it != end; it.increment(error))
			{
				if (it->path().extension() == ".xml")
					files.push_back(it->path());
			}
			std::sort(files.begin(), files.end());
			if (_macro.empty() && files.empty())
			{
				setStatusMessage("Grave uma macro primeiro (Macro → Iniciar gravação).");
				break;
			}
			GtkWidget *dialog = gtk_dialog_new_with_buttons("Executar macro várias vezes", parent, GTK_DIALOG_MODAL,
				"_Cancelar", GTK_RESPONSE_CANCEL, "_Executar", GTK_RESPONSE_ACCEPT, nullptr);
			GtkWidget *grid = gtk_grid_new();
			gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
			gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
			gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
			GtkWidget *which = gtk_combo_box_text_new();
			if (!_macro.empty())
				gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(which), "Macro gravada atual");
			for (const auto &file : files)
				gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(which), file.stem().c_str());
			gtk_combo_box_set_active(GTK_COMBO_BOX(which), 0);
			GtkWidget *times = gtk_radio_button_new_with_label(nullptr, "Executar");
			GtkWidget *count = gtk_spin_button_new_with_range(1, 100000, 1);
			GtkWidget *untilEnd = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(times), "Executar até o fim do arquivo");
			GtkWidget *macroLabel = gtk_label_new("Macro:");
			gtk_label_set_xalign(GTK_LABEL(macroLabel), 0.0);
			gtk_grid_attach(GTK_GRID(grid), macroLabel, 0, 0, 1, 1);
			gtk_grid_attach(GTK_GRID(grid), which, 1, 0, 2, 1);
			gtk_grid_attach(GTK_GRID(grid), times, 0, 1, 1, 1);
			gtk_grid_attach(GTK_GRID(grid), count, 1, 1, 1, 1);
			gtk_grid_attach(GTK_GRID(grid), gtk_label_new("vezes"), 2, 1, 1, 1);
			gtk_grid_attach(GTK_GRID(grid), untilEnd, 0, 2, 3, 1);
			gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid);
			gtk_widget_show_all(dialog);
			const bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
			int index = gtk_combo_box_get_active(GTK_COMBO_BOX(which));
			const std::size_t repetitions = static_cast<std::size_t>(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(count)));
			const bool toEnd = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(untilEnd));
			gtk_widget_destroy(dialog);
			if (!accepted)
				break;
			if (!_macro.empty() && index == 0)
				runMacroCommands(_macro, repetitions, toEnd);
			else
			{
				index -= _macro.empty() ? 0 : 1;
				if (index >= 0 && static_cast<std::size_t>(index) < files.size())
					runMacroFile(files[static_cast<std::size_t>(index)], repetitions, toEnd);
			}
			break;
		}

		// Executar
		case Action::RunCommand:
			showRunDialog();
			break;
		case Action::OpenInBrowser:
			if (needPath() && !_selfTesting)
			{
				GAppInfo *browser = g_app_info_get_default_for_uri_scheme("https");
				GList *uris = g_list_append(nullptr, g_strdup(fileUri(document->path).c_str()));
				GError *gerror = nullptr;
				const bool opened = browser && g_app_info_launch_uris(browser, uris, nullptr, &gerror);
				g_list_free_full(uris, g_free);
				if (browser)
					g_object_unref(browser);
				if (!opened)
					showError(std::string("Não foi possível abrir o navegador.\n\n") + (gerror ? gerror->message : "Nenhum navegador padrão."));
				if (gerror)
					g_error_free(gerror);
			}
			break;

		// Janela
		case Action::WindowsList:
			showWindowsList();
			return;
		case Action::SortTabsNameAsc: case Action::SortTabsNameDesc: case Action::SortTabsPathAsc: case Action::SortTabsPathDesc:
		case Action::SortTabsTypeAsc: case Action::SortTabsTypeDesc: case Action::SortTabsSizeAsc: case Action::SortTabsSizeDesc:
		case Action::SortTabsTimeAsc: case Action::SortTabsTimeDesc:
			sortTabs(action);
			return;

		// Funções dos plugins do Windows, embutidas.
		case Action::CompareWith:
		{
			if (!document)
				break;
			std::vector<EditorDocument *> others;
			for (const auto &owned : _documents)
			{
				if (owned.get() != document)
					others.push_back(owned.get());
			}
			if (others.empty())
			{
				setStatusMessage("Abra outro documento para comparar.");
				break;
			}
			EditorDocument *other = others.size() == 1 ? others.front() : nullptr;
			if (!other && !_selfTesting)
			{
				GtkWidget *dialog = gtk_dialog_new_with_buttons("Comparar", parent, GTK_DIALOG_MODAL,
					"_Cancelar", GTK_RESPONSE_CANCEL, "_Comparar", GTK_RESPONSE_ACCEPT, nullptr);
				GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
				gtk_container_set_border_width(GTK_CONTAINER(box), 12);
				const std::string caption = "Comparar “" + document->displayName + "” com:";
				GtkWidget *label = gtk_label_new(caption.c_str());
				gtk_label_set_xalign(GTK_LABEL(label), 0.0);
				GtkWidget *combo = gtk_combo_box_text_new();
				for (EditorDocument *candidate : others)
					gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), candidate->displayName.c_str());
				gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
				gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
				gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);
				gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box);
				gtk_widget_show_all(dialog);
				const bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
				const int chosen = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
				gtk_widget_destroy(dialog);
				if (accepted && chosen >= 0)
					other = others[static_cast<std::size_t>(chosen)];
			}
			if (other)
				compareDocuments(*document, *other, true);
			return;
		}
		case Action::CompareNext:
		case Action::ComparePrevious:
			if (document)
			{
				const sptr_t count = document->send(SCI_GETLINECOUNT);
				const sptr_t line = currentLine(*document);
				auto changed = [&](sptr_t l) { return l >= 0 && l < count && (document->send(SCI_MARKERGET, l) & CompareMask) != 0; };
				const bool forward = action == Action::CompareNext;
				for (sptr_t step = 1; step <= count; ++step)
				{
					const sptr_t candidate = ((forward ? line + step : line - step) % count + count) % count;
					if (changed(candidate) && !changed(candidate - 1))
					{
						document->send(SCI_ENSUREVISIBLEENFORCEPOLICY, candidate);
						document->send(SCI_GOTOLINE, candidate);
						return;
					}
				}
				setStatusMessage("Não há diferenças marcadas neste documento.");
			}
			break;
		case Action::CompareClear:
			clearComparison();
			break;
		case Action::Base64Encode:
			if (document)
				applyTextTool(*document, [](const std::string &in, std::string &out) { out = base64Encode(in); return true; }, "");
			break;
		case Action::Base64Decode:
			if (document)
				applyTextTool(*document, [](const std::string &in, std::string &out) { return base64Decode(in, out); }, "O texto não é Base64 válido.");
			break;
		case Action::UrlEncode:
			if (document)
				applyTextTool(*document, [](const std::string &in, std::string &out) { out = urlEncode(in); return true; }, "");
			break;
		case Action::UrlDecode:
			if (document)
				applyTextTool(*document, [](const std::string &in, std::string &out) { return urlDecode(in, out); }, "O texto não é uma URL codificada válida.");
			break;
		case Action::AsciiToHex:
			if (document)
				applyTextTool(*document, [](const std::string &in, std::string &out) { out = asciiToHex(in); return true; }, "");
			break;
		case Action::HexToAscii:
			if (document)
				applyTextTool(*document, [](const std::string &in, std::string &out) { return hexToAscii(in, out); }, "O texto não é hexadecimal válido (pares de 0-9 e A-F).");
			break;
		case Action::JsMinify:
			if (document)
			{
				std::string error;
				if (!applyTextTool(*document, [&error](const std::string &in, std::string &out) { return jsMinify(in, out, error); }, "") && !error.empty())
					setStatusMessage("Não foi possível minificar: " + error);
			}
			break;
		case Action::JsFormat:
			if (document)
			{
				const std::string indent = _settings.useTabs ? std::string("\t") : std::string(static_cast<std::size_t>(_settings.tabWidth), ' ');
				applyTextTool(*document, [&indent](const std::string &in, std::string &out) { out = jsFormat(in, indent); return true; }, "");
			}
			break;
		case Action::ExportHtml:
		case Action::ExportRtf:
			if (document && !_selfTesting)
			{
				const bool html = action == Action::ExportHtml;
				const std::string base = document->path.empty() ? document->displayName : document->path.stem().string();
				const auto target = chooseFile(parent, html ? "Exportar para HTML" : "Exportar para RTF", GTK_FILE_CHOOSER_ACTION_SAVE, "_Exportar",
					document->path.empty() ? std::filesystem::path() : document->path.parent_path(), base + (html ? ".html" : ".rtf"));
				if (target.empty())
					break;
				const std::string content = html ? documentHtml(*document) : documentRtf(*document);
				std::ofstream output(target, std::ios::binary | std::ios::trunc);
				output << content;
				if (!output)
					showError("Não foi possível gravar " + target.string());
				else
					setStatusMessage("Exportado para " + target.string());
			}
			break;

		// Itens confirmados no menu da versão Windows 8.9.7 instalada.
		case Action::RestoreClosedFile:
			while (!_closedFiles.empty())
			{
				const std::string path = _closedFiles.back();
				_closedFiles.pop_back();
				std::error_code error;
				if (std::filesystem::is_regular_file(path, error))
				{
					openPath(path);
					return;
				}
			}
			setStatusMessage("Nenhum arquivo fechado para restaurar.");
			break;
		case Action::OpenAllRecent:
		{
			const auto recent = _settings.recentFiles;
			for (const auto &path : recent)
			{
				std::error_code error;
				if (std::filesystem::is_regular_file(path, error))
					openPath(path);
			}
			return;
		}
		case Action::ClearRecentList:
			_settings.recentFiles.clear();
			saveSettings();
			// The menu holding this item is rebuilt once the activation has finished.
			g_idle_add(+[](gpointer data) -> gboolean {
				static_cast<EditorWindow *>(data)->rebuildRecentMenu();
				return G_SOURCE_REMOVE;
			}, this);
			break;
		case Action::BeginEndSelect:
		case Action::BeginEndSelectColumn:
			if (document)
			{
				const sptr_t position = document->send(SCI_GETCURRENTPOS);
				if (document->selectionMark < 0)
				{
					document->selectionMark = position;
					setStatusMessage("Início da seleção marcado. Vá até o fim e use o mesmo comando de novo.");
					break;
				}
				const sptr_t mark = std::min(document->selectionMark, document->send(SCI_GETLENGTH));
				document->selectionMark = -1;
				if (action == Action::BeginEndSelectColumn)
				{
					document->send(SCI_SETRECTANGULARSELECTIONANCHOR, mark);
					document->send(SCI_SETRECTANGULARSELECTIONCARET, position);
				}
				else
					document->send(SCI_SETSEL, mark, position);
			}
			break;
		case Action::ToggleMacroRecord:
			perform(_recordingMacro ? Action::StopMacro : Action::StartMacro);
			return;
		case Action::FindVolatileNext:
		case Action::FindVolatilePrevious:
			if (document)
			{
				// Searches the word or selection without changing the search box.
				const std::string needle = currentWordOrSelection(*document);
				if (needle.empty())
					break;
				const bool forward = action == Action::FindVolatileNext;
				const sptr_t length = document->send(SCI_GETLENGTH);
				const sptr_t from = forward ? document->send(SCI_GETSELECTIONEND) : document->send(SCI_GETSELECTIONSTART);
				document->send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
				auto search = [&](sptr_t start, sptr_t end) {
					document->send(SCI_SETTARGETRANGE, start, end);
					return document->send(SCI_SEARCHINTARGET, needle.size(), reinterpret_cast<sptr_t>(needle.c_str()));
				};
				sptr_t found = forward ? search(from, length) : search(from, 0);
				if (found < 0)
					found = forward ? search(0, length) : search(length, 0);
				if (found >= 0)
				{
					document->send(SCI_SETSEL, found, document->send(SCI_GETTARGETEND));
					document->send(SCI_SCROLLCARET);
				}
			}
			break;
		case Action::StyleOneToken1: case Action::StyleOneToken2: case Action::StyleOneToken3: case Action::StyleOneToken4: case Action::StyleOneToken5:
			if (document)
			{
				sptr_t start = document->send(SCI_GETSELECTIONSTART);
				sptr_t end = document->send(SCI_GETSELECTIONEND);
				if (end <= start)
				{
					const sptr_t position = document->send(SCI_GETCURRENTPOS);
					start = document->send(SCI_WORDSTARTPOSITION, position, 1);
					end = document->send(SCI_WORDENDPOSITION, position, 1);
				}
				if (end > start)
				{
					document->send(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(FirstTokenIndicator + static_cast<int>(action) - static_cast<int>(Action::StyleOneToken1)));
					document->send(SCI_INDICATORFILLRANGE, start, end - start);
				}
			}
			break;
		case Action::CopyStyled1: case Action::CopyStyled2: case Action::CopyStyled3: case Action::CopyStyled4: case Action::CopyStyled5:
		case Action::CopyStyledAll:
			if (document)
			{
				const int style = action == Action::CopyStyledAll ? -1 : static_cast<int>(action) - static_cast<int>(Action::CopyStyled1);
				const std::string text = styledText(*document, style);
				if (text.empty())
					setStatusMessage("Não há texto com esse destaque.");
				else
				{
					copyToClipboard(text);
					setStatusMessage("Texto destacado copiado.");
				}
			}
			break;
		case Action::PasteReplaceBookmarked:
			if (document)
			{
				gchar *clip = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
				const std::string text = clip ? clip : "";
				g_free(clip);
				std::vector<std::string> pieces;
				std::istringstream input(text);
				std::string piece;
				while (std::getline(input, piece))
				{
					if (!piece.empty() && piece.back() == '\r')
						piece.pop_back();
					pieces.push_back(piece);
				}
				const auto marked = bookmarkedLines(true);
				if (pieces.empty() || marked.empty())
				{
					setStatusMessage("É preciso ter linhas marcadas e texto copiado.");
					break;
				}
				document->send(SCI_BEGINUNDOACTION);
				for (std::size_t i = 0; i < marked.size() && i < pieces.size(); ++i)
				{
					document->send(SCI_SETTARGETRANGE, document->send(SCI_POSITIONFROMLINE, marked[i]), document->send(SCI_GETLINEENDPOSITION, marked[i]));
					document->send(SCI_REPLACETARGET, pieces[i].size(), reinterpret_cast<sptr_t>(pieces[i].c_str()));
				}
				document->send(SCI_ENDUNDOACTION);
			}
			break;

		// Ajuda
		case Action::CommandLineHelp:
			showTextDialog("Argumentos de linha de comando",
				"notepad-plus-plus-linux [opções] [arquivos...]\n\n"
				"  --help                 Mostra a ajuda\n"
				"  --version              Mostra a versão\n"
				"  --no-session           Não restaura a sessão anterior nesta execução\n"
				"  --self-test <pasta>    Testa a janela, a edição, a busca e os arquivos\n"
				"  --recovery-smoke-test <pasta>  Testa a recuperação entre duas execuções\n");
			break;
		case Action::DebugInfo:
		{
			std::ostringstream info;
			info << "Notepad++ Linux 0.7.0 (GTK " << gtk_get_major_version() << "." << gtk_get_minor_version() << "." << gtk_get_micro_version()
				<< ", GLib " << glib_major_version << "." << glib_minor_version << "." << glib_micro_version << ")\n"
				<< "Sistema: " << osName() << "\n"
				<< "Ambiente: " << (g_getenv("XDG_CURRENT_DESKTOP") ? g_getenv("XDG_CURRENT_DESKTOP") : "?")
				<< " / " << (g_getenv("XDG_SESSION_TYPE") ? g_getenv("XDG_SESSION_TYPE") : "?") << "\n"
				<< "Configurações: " << _settingsStore.path().parent_path().string() << "\n"
				<< "Pastas de plugins:\n";
			for (const auto &folder : PluginManager::defaultDirectories())
				info << "  " << folder.string() << "\n";
			info << "Documentos abertos: " << _documents.size() << "\n";
			showTextDialog("Informações de depuração", info.str());
			break;
		}
		default:
			break;
	}
}

bool EditorWindow::applyTextTool(EditorDocument &document, const std::function<bool(const std::string &, std::string &)> &tool, const std::string &failure)
{
	// Works on the selection, or on the whole document when nothing is selected.
	sptr_t start = document.send(SCI_GETSELECTIONSTART);
	sptr_t end = document.send(SCI_GETSELECTIONEND);
	const bool whole = end <= start;
	if (whole)
	{
		start = 0;
		end = document.send(SCI_GETLENGTH);
	}
	std::string result;
	if (!tool(textRange(document, start, end), result))
	{
		if (!failure.empty())
			setStatusMessage(failure);
		return false;
	}
	if (!g_utf8_validate(result.data(), static_cast<gssize>(result.size()), nullptr) || result.find('\0') != std::string::npos)
	{
		setStatusMessage("O resultado não é texto (são dados binários) e não foi inserido.");
		return false;
	}
	document.send(SCI_BEGINUNDOACTION);
	document.send(SCI_SETTARGETRANGE, start, end);
	document.send(SCI_REPLACETARGET, result.size(), reinterpret_cast<sptr_t>(result.c_str()));
	if (!whole)
		document.send(SCI_SETSEL, start, start + static_cast<sptr_t>(result.size()));
	document.send(SCI_ENDUNDOACTION);
	return true;
}

std::size_t EditorWindow::compareDocuments(EditorDocument &left, EditorDocument &right, bool report)
{
	clearComparison();
	const auto leftLines = splitLines(left.text());
	const auto rightLines = splitLines(right.text());
	std::vector<std::string> leftKeys;
	std::vector<std::string> rightKeys;
	for (const auto &line : leftLines)
		leftKeys.push_back(compareKey(line, _compareIgnoreSpaces, _compareIgnoreCase));
	for (const auto &line : rightLines)
		rightKeys.push_back(compareKey(line, _compareIgnoreSpaces, _compareIgnoreCase));
	auto diff = diffLines(leftKeys, rightKeys);
	std::size_t blocks = 0;
	char previous = ' ';
	for (auto &line : diff)
	{
		line.text = line.kind == '+' ? rightLines[line.right] : leftLines[line.left];
		if (line.kind == '-')
			left.send(SCI_MARKERADD, line.left, CompareRemovedMarker);
		else if (line.kind == '+')
			right.send(SCI_MARKERADD, line.right, CompareAddedMarker);
		if (line.kind != ' ' && previous == ' ')
			++blocks;
		previous = line.kind;
	}
	// The two documents side by side, like ComparePlus on Windows.
	if (report)
		showCompareView(left, right, diff);
	if (blocks == 0)
	{
		setStatusMessage("Os documentos são iguais.");
		return 0;
	}
	setStatusMessage(std::to_string(blocks) + " diferença(s): em vermelho o que só existe em “" + left.displayName
		+ "”, em verde o que só existe em “" + right.displayName + "”. Alt+PgDn vai para a próxima.");
	return blocks;
}

void EditorWindow::clearComparison()
{
	for (const auto &owned : _documents)
	{
		owned->send(SCI_MARKERDELETEALL, CompareRemovedMarker);
		owned->send(SCI_MARKERDELETEALL, CompareAddedMarker);
	}
}

namespace
{
struct StyleColours
{
	int fore;
	int back;
	bool bold;
	bool italic;
};

std::string htmlColour(int colour)
{
	char buffer[8];
	std::snprintf(buffer, sizeof buffer, "#%02x%02x%02x", colour & 0xff, (colour >> 8) & 0xff, (colour >> 16) & 0xff);
	return buffer;
}

std::vector<std::pair<int, std::string>> styleRuns(EditorDocument &document)
{
	document.send(SCI_COLOURISE, 0, -1);
	const std::string text = document.text();
	std::vector<std::pair<int, std::string>> runs;
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		const int style = static_cast<int>(document.send(SCI_GETSTYLEAT, static_cast<uptr_t>(i)));
		if (runs.empty() || runs.back().first != style)
			runs.emplace_back(style, std::string());
		runs.back().second += text[i];
	}
	return runs;
}

StyleColours coloursOf(EditorDocument &document, int style)
{
	return {static_cast<int>(document.send(SCI_STYLEGETFORE, static_cast<uptr_t>(style))),
		static_cast<int>(document.send(SCI_STYLEGETBACK, static_cast<uptr_t>(style))),
		document.send(SCI_STYLEGETBOLD, static_cast<uptr_t>(style)) != 0,
		document.send(SCI_STYLEGETITALIC, static_cast<uptr_t>(style)) != 0};
}
}

std::string EditorWindow::documentHtml(EditorDocument &document)
{
	const auto runs = styleRuns(document);
	std::set<int> used;
	for (const auto &run : runs)
		used.insert(run.first);
	auto escape = [](const std::string &text) {
		std::string out;
		for (const char c : text)
		{
			switch (c)
			{
				case '&': out += "&amp;"; break;
				case '<': out += "&lt;"; break;
				case '>': out += "&gt;"; break;
				case '"': out += "&quot;"; break;
				case '\r': break;
				default: out += c;
			}
		}
		return out;
	};
	const StyleColours base = coloursOf(document, STYLE_DEFAULT);
	std::ostringstream html;
	html << "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>" << escape(document.displayName) << "</title>\n<style>\n"
		<< "body { background: " << htmlColour(base.back) << "; color: " << htmlColour(base.fore) << "; }\n"
		<< "pre { font-family: '" << escape(_settings.fontFamily) << "', monospace; font-size: " << _settings.fontSize << "pt; tab-size: " << _settings.tabWidth << "; }\n";
	for (const int style : used)
	{
		const StyleColours colours = coloursOf(document, style);
		html << ".s" << style << " { color: " << htmlColour(colours.fore) << "; background: " << htmlColour(colours.back) << ";"
			<< (colours.bold ? " font-weight: bold;" : "") << (colours.italic ? " font-style: italic;" : "") << " }\n";
	}
	html << "</style>\n</head>\n<body>\n<pre>";
	for (const auto &run : runs)
		html << "<span class=\"s" << run.first << "\">" << escape(run.second) << "</span>";
	html << "</pre>\n</body>\n</html>\n";
	return html.str();
}

std::string EditorWindow::documentRtf(EditorDocument &document)
{
	const auto runs = styleRuns(document);
	std::vector<int> colours;
	auto colourIndex = [&](int colour) {
		const auto found = std::find(colours.begin(), colours.end(), colour);
		if (found != colours.end())
			return static_cast<int>(found - colours.begin()) + 1;
		colours.push_back(colour);
		return static_cast<int>(colours.size());
	};
	std::ostringstream body;
	for (const auto &run : runs)
	{
		const StyleColours style = coloursOf(document, run.first);
		body << "\\cf" << colourIndex(style.fore) << "\\highlight" << colourIndex(style.back) << (style.bold ? "\\b " : "\\b0 ") << (style.italic ? "\\i " : "\\i0 ");
		for (const char *p = run.second.c_str(); *p; p = g_utf8_next_char(p))
		{
			const gunichar c = g_utf8_get_char(p);
			if (c == '\r')
				continue;
			if (c == '\n')
				body << "\\par\n";
			else if (c == '\t')
				body << "\\tab ";
			else if (c == '\\' || c == '{' || c == '}')
				body << '\\' << static_cast<char>(c);
			else if (c < 128)
				body << static_cast<char>(c);
			else if (c <= 0xFFFF)
				body << "\\u" << static_cast<int>(static_cast<std::int16_t>(c)) << '?';
			else
			{
				// Characters outside the basic plane are written as a UTF-16 surrogate pair.
				const gunichar value = c - 0x10000;
				body << "\\u" << static_cast<int>(static_cast<std::int16_t>(0xD800 + (value >> 10))) << '?'
					<< "\\u" << static_cast<int>(static_cast<std::int16_t>(0xDC00 + (value & 0x3FF))) << '?';
			}
		}
	}
	std::ostringstream rtf;
	rtf << "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fmodern " << _settings.fontFamily << ";}}\n{\\colortbl ;";
	for (const int colour : colours)
		rtf << "\\red" << (colour & 0xff) << "\\green" << ((colour >> 8) & 0xff) << "\\blue" << ((colour >> 16) & 0xff) << ';';
	rtf << "}\n\\f0\\fs" << _settings.fontSize * 2 << ' ' << body.str() << "\n}\n";
	return rtf.str();
}

std::string EditorWindow::styledText(EditorDocument &document, int style) const
{
	std::string text;
	const std::string eol = eolString(document);
	for (const auto &range : tokenRanges(document))
	{
		if (style >= 0 && range.indicator != FirstTokenIndicator + style)
			continue;
		text += (text.empty() ? "" : eol) + textRange(document, range.start, range.end);
	}
	return text;
}

std::vector<std::string> EditorWindow::completionWords(EditorDocument &document) const
{
	const sptr_t position = document.send(SCI_GETCURRENTPOS);
	const sptr_t start = document.send(SCI_WORDSTARTPOSITION, position, 1);
	const std::string prefix = textRange(document, start, position);
	if (prefix.empty())
		return {};
	gchar *foldedPrefix = g_utf8_casefold(prefix.c_str(), -1);
	const std::string lowerPrefix = foldedPrefix ? foldedPrefix : prefix;
	g_free(foldedPrefix);
	const std::string text = document.text();
	std::set<std::string> words;
	std::string word;
	auto flush = [&]() {
		if (word.size() >= prefix.size() && word != prefix)
		{
			gchar *folded = g_utf8_casefold(word.c_str(), -1);
			if (folded && std::string(folded).rfind(lowerPrefix, 0) == 0)
				words.insert(word);
			g_free(folded);
		}
		word.clear();
	};
	for (const char *p = text.c_str(); *p; p = g_utf8_next_char(p))
	{
		const gunichar c = g_utf8_get_char(p);
		if (g_unichar_isalnum(c) || c == '_')
			word.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
		else
			flush();
	}
	flush();
	return {words.begin(), words.end()};
}

bool EditorWindow::renameDocument(EditorDocument &document, const std::string &name)
{
	if (document.path.empty())
	{
		document.displayName = name;
		document.language = &languageFor(name);
		applyTheme(document);
		saveSession();
		refreshDocument(&document);
		return true;
	}
	const auto target = document.path.parent_path() / name;
	std::error_code error;
	if (std::filesystem::exists(target, error))
	{
		showError("Já existe um arquivo com esse nome.");
		return false;
	}
	std::filesystem::rename(document.path, target, error);
	if (error)
	{
		showError("Não foi possível renomear.\n\n" + error.message());
		return false;
	}
	document.path = target;
	document.displayName = name;
	document.language = &languageFor(target);
	applyTheme(document);
	addRecentFile(target);
	saveSession();
	refreshDocument(&document);
	return true;
}
}
