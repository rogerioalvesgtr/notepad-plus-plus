#include "EditorWindow.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include <sys/stat.h>
#include <unistd.h>
#include <gdk/gdkkeysyms.h>

#include "ILexer.h"
#include "Lexilla.h"
#include "SciLexer.h"
#include "Scintilla.h"
#include "ScintillaWidget.h"
#include "SessionStore.h"
#include "TextTools.h"
#include "BuiltinTools.h"
#include "Integration.h"
#include "NppStyles.h"

namespace NppLinux
{
namespace
{
constexpr int TabNameLimit = 30;

constexpr int colour(int red, int green, int blue)
{
	return red | (green << 8) | (blue << 16);
}

void attachData(GtkWidget *object, const char *name, gpointer value);

void actionActivated(GtkWidget *item, gpointer data)
{
	const auto action = static_cast<Action>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "npp-action")));
	static_cast<EditorWindow *>(data)->perform(action);
}

void connectAction(GtkWidget *widget, const char *signal, EditorWindow *window, Action action)
{
	attachData(widget, "npp-action", GINT_TO_POINTER(static_cast<int>(action)));
	g_signal_connect(widget, signal, G_CALLBACK(actionActivated), window);
}

void pageSwitched(GtkNotebook *, GtkWidget *, guint page, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	window->moveRecordingToCurrentPage();
	window->refreshDocument();
	window->updateMacroState();
	(void)page;
}

void pageReordered(GtkNotebook *, GtkWidget *page, guint destination, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	const gint origin = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "npp-page-index"));
	window->reorderDocument(origin, static_cast<gint>(destination));
	GtkNotebook *notebook = GTK_NOTEBOOK(g_object_get_data(G_OBJECT(page), "npp-notebook"));
	const gint pages = gtk_notebook_get_n_pages(notebook);
	for (gint i = 0; i < pages; ++i)
	{
		GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
		g_object_set_data(G_OBJECT(child), "npp-page-index", GINT_TO_POINTER(i));
	}
}

void notificationReceived(ScintillaObject *, int, SCNotification *notification, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	if (notification->nmhdr.code == SCN_MACRORECORD)
	{
		window->recordMacro(notification->message, notification->wParam, notification->lParam, notification->text);
		return;
	}
	if (notification->nmhdr.code == SCN_STYLENEEDED || notification->nmhdr.code == SCN_CHARADDED)
	{
		auto *document = static_cast<EditorDocument *>(g_object_get_data(G_OBJECT(notification->nmhdr.hwndFrom), "npp-document"));
		if (notification->nmhdr.code == SCN_STYLENEEDED)
			window->styleNeeded(document, notification->position);
		else
			window->charAdded(document, notification->ch);
		return;
	}
	if (notification->nmhdr.code == SCN_MARGINCLICK)
	{
		auto *document = static_cast<EditorDocument *>(g_object_get_data(G_OBJECT(notification->nmhdr.hwndFrom), "npp-document"));
		window->marginClicked(document, notification->margin, notification->position);
		return;
	}
	if (notification->nmhdr.code == SCN_MODIFIED
		&& (notification->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)))
		window->scheduleFunctionList();
	if (notification->nmhdr.code == SCN_MODIFIED
		|| notification->nmhdr.code == SCN_SAVEPOINTREACHED
		|| notification->nmhdr.code == SCN_SAVEPOINTLEFT
		|| notification->nmhdr.code == SCN_UPDATEUI
		|| notification->nmhdr.code == SCN_ZOOM)
	{
		auto *document = static_cast<EditorDocument *>(g_object_get_data(G_OBJECT(notification->nmhdr.hwndFrom), "npp-document"));
		window->refreshDocument(document);
	}
}

void closeTabClicked(GtkButton *, gpointer data)
{
	auto *document = static_cast<EditorDocument *>(data);
	auto *window = static_cast<EditorWindow *>(g_object_get_data(G_OBJECT(document->widget()), "npp-window"));
	const int page = gtk_notebook_page_num(GTK_NOTEBOOK(g_object_get_data(G_OBJECT(document->widget()), "npp-notebook")), document->widget());
	if (page >= 0)
		window->closeDocument(static_cast<std::size_t>(page));
}

void openRecentActivated(GtkMenuItem *, gpointer data)
{
	const auto *path = static_cast<const std::string *>(g_object_get_data(G_OBJECT(data), "npp-recent-path"));
	auto *window = static_cast<EditorWindow *>(g_object_get_data(G_OBJECT(data), "npp-window"));
	if (path && window)
		window->openPath(*path);
}

void languageActivated(GtkCheckMenuItem *item, gpointer data)
{
	if (!gtk_check_menu_item_get_active(item))
		return;
	const auto *language = static_cast<const Language *>(g_object_get_data(G_OBJECT(data), "npp-language"));
	auto *window = static_cast<EditorWindow *>(g_object_get_data(G_OBJECT(data), "npp-window"));
	if (language && window)
		window->chooseLanguage(*language);
}

gboolean windowDelete(GtkWidget *, GdkEvent *, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	window->perform(Action::Quit);
	return TRUE;
}

void searchActivated(GtkWidget *widget, gpointer data)
{
	const int operation = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "npp-search-operation"));
	static_cast<EditorWindow *>(data)->showFindResults(operation == 0 ? "next" : operation == 1 ? "back" : operation == 2 ? "replace" : operation == 3 ? "replaceAll" : "count");
}

void hideSearch(GtkWidget *, gpointer data)
{
	auto *searchBar = GTK_WIDGET(g_object_get_data(G_OBJECT(data), "npp-search-bar"));
	gtk_widget_hide(searchBar);
	auto *document = static_cast<EditorWindow *>(g_object_get_data(G_OBJECT(data), "npp-window"))->currentDocument();
	if (document)
		gtk_widget_grab_focus(document->widget());
}

void attachData(GtkWidget *object, const char *name, gpointer value)
{
	g_object_set_data(G_OBJECT(object), name, value);
}

void toolActionActivated(GtkToolItem *item, gpointer data)
{
	const auto action = static_cast<Action>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "npp-action")));
	static_cast<EditorWindow *>(data)->perform(action);
}

GtkWidget *menuItem(GtkWidget *menu, const char *label, EditorWindow *window,
	Action action, GtkAccelGroup *accelerators, guint key = 0,
	GdkModifierType modifiers = static_cast<GdkModifierType>(0))
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	connectAction(item, "activate", window, action);
	if (key)
		gtk_widget_add_accelerator(item, "activate", accelerators, key, modifiers, GTK_ACCEL_VISIBLE);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

GtkWidget *checkItem(GtkWidget *menu, const char *label, EditorWindow *window,
	Action action, bool active, GtkAccelGroup *accelerators, guint key = 0,
	GdkModifierType modifiers = static_cast<GdkModifierType>(0))
{
	GtkWidget *item = gtk_check_menu_item_new_with_mnemonic(label);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);
	connectAction(item, "activate", window, action);
	if (key)
		gtk_widget_add_accelerator(item, "activate", accelerators, key, modifiers, GTK_ACCEL_VISIBLE);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

void submenu(GtkWidget *bar, const char *label, GtkWidget *menu)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(bar), item);
}

GtkToolItem *toolButton(const char *stock, const char *label, const char *tooltip,
	EditorWindow *window, Action action)
{
	GtkToolItem *button = gtk_tool_button_new_from_stock(stock);
	gtk_tool_button_set_label(GTK_TOOL_BUTTON(button), label);
	gtk_tool_item_set_tooltip_text(button, tooltip);
	attachData(GTK_WIDGET(button), "npp-action", GINT_TO_POINTER(static_cast<int>(action)));
	g_signal_connect(button, "clicked", G_CALLBACK(toolActionActivated), window);
	return button;
}

GtkWidget *labelledEntry(GtkBox *box, const char *label, const char *placeholder, int width)
{
	GtkWidget *caption = gtk_label_new(label);
	gtk_box_pack_start(box, caption, FALSE, FALSE, 0);
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
	gtk_entry_set_width_chars(GTK_ENTRY(entry), width);
	gtk_box_pack_start(box, entry, TRUE, TRUE, 0);
	return entry;
}

void style(EditorDocument &document, int id, int foreground, bool bold = false, bool italic = false)
{
	document.send(SCI_STYLESETFORE, id, foreground);
	document.send(SCI_STYLESETBOLD, id, bold ? 1 : 0);
	document.send(SCI_STYLESETITALIC, id, italic ? 1 : 0);
}

gboolean backupTimer(gpointer data)
{
	return static_cast<EditorWindow *>(data)->backupTick() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

enum ResultColumn
{
	ResultText,
	ResultIndex,
	ResultWeight,
	ResultColumns
};

enum FindInFilesResponse
{
	FindAllInFiles = 1,
	ReplaceAllInFiles = 2,
	FindAllInOpenDocuments = 3,
	ReplaceAllInOpenDocuments = 4
};

void resultRowActivated(GtkTreeView *, GtkTreePath *path, GtkTreeViewColumn *, gpointer data)
{
	static_cast<EditorWindow *>(data)->activateResultRow(path);
}

void findDialogResponse(GtkDialog *, gint response, gpointer data)
{
	static_cast<EditorWindow *>(data)->runFindInFilesDialog(response);
}

void chooseFolderClicked(GtkButton *, gpointer data)
{
	static_cast<EditorWindow *>(data)->chooseFindFolder();
}

void closeResultsClicked(GtkButton *, gpointer data)
{
	static_cast<EditorWindow *>(data)->hideResults();
}

enum FunctionColumn
{
	FunctionText,
	FunctionKind,
	FunctionIndex,
	FunctionWeight,
	FunctionColumns
};

enum WorkspaceColumn
{
	WorkspaceName,
	WorkspacePath,
	WorkspaceDirectory,
	WorkspaceLoaded,
	WorkspaceIcon,
	WorkspaceColumns
};

void functionRowActivated(GtkTreeView *, GtkTreePath *path, GtkTreeViewColumn *, gpointer data)
{
	static_cast<EditorWindow *>(data)->activateFunctionRow(path);
}

void functionFilterChanged(GtkEditable *, gpointer data)
{
	static_cast<EditorWindow *>(data)->refreshFunctionList();
}

void functionSortToggled(GtkToggleButton *, gpointer data)
{
	static_cast<EditorWindow *>(data)->refreshFunctionList();
}

gboolean functionTimerFired(gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	window->_functionTimer = 0;
	window->refreshFunctionList();
	return G_SOURCE_REMOVE;
}

gboolean mapButtonPressed(GtkWidget *, GdkEventButton *event, gpointer data)
{
	if (event->button == 1)
		static_cast<EditorWindow *>(data)->documentMapJumpToPoint(event->x, event->y);
	return TRUE;
}

gboolean mapMotion(GtkWidget *, GdkEventMotion *event, gpointer data)
{
	if (event->state & GDK_BUTTON1_MASK)
		static_cast<EditorWindow *>(data)->documentMapJumpToPoint(event->x, event->y);
	return TRUE;
}

gboolean mapScrolled(GtkWidget *, GdkEventScroll *event, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	if (event->direction == GDK_SCROLL_UP)
		window->documentMapScroll(-3);
	else if (event->direction == GDK_SCROLL_DOWN)
		window->documentMapScroll(3);
	else if (event->direction == GDK_SCROLL_SMOOTH && event->delta_y != 0)
		window->documentMapScroll(event->delta_y > 0 ? 3 : -3);
	return TRUE;
}

gboolean swallowEvent(GtkWidget *, GdkEvent *, gpointer)
{
	return TRUE;
}

void workspaceRowActivated(GtkTreeView *, GtkTreePath *path, GtkTreeViewColumn *, gpointer data)
{
	static_cast<EditorWindow *>(data)->activateWorkspaceRow(path);
}

gboolean workspaceTestExpand(GtkTreeView *, GtkTreeIter *iter, GtkTreePath *, gpointer data)
{
	static_cast<EditorWindow *>(data)->populateWorkspaceNode(iter);
	return FALSE;
}

void workspaceAddClicked(GtkButton *, gpointer data)
{
	static_cast<EditorWindow *>(data)->chooseWorkspaceFolder();
}

void workspaceRemoveClicked(GtkButton *, gpointer data)
{
	static_cast<EditorWindow *>(data)->removeSelectedWorkspaceFolder();
}

void workspaceRefreshClicked(GtkButton *, gpointer data)
{
	static_cast<EditorWindow *>(data)->rebuildWorkspace();
}

void workspaceFindClicked(GtkButton *, gpointer data)
{
	static_cast<EditorWindow *>(data)->findInSelectedWorkspaceFolder();
}

void pageSwitchedAfter(GtkNotebook *, GtkWidget *, guint, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	window->scheduleFunctionList(true);
	window->syncDocumentMap();
}

intptr_t hostSendEditor(void *context, unsigned int message, uintptr_t wParam, intptr_t lParam)
{
	EditorDocument *document = static_cast<EditorWindow *>(context)->currentDocument();
	return document ? document->send(message, wParam, lParam) : 0;
}

char *hostGetText(void *context)
{
	EditorDocument *document = static_cast<EditorWindow *>(context)->currentDocument();
	return g_strdup(document ? document->text().c_str() : "");
}

char *hostGetSelection(void *context)
{
	EditorDocument *document = static_cast<EditorWindow *>(context)->currentDocument();
	if (!document)
		return g_strdup("");
	const sptr_t length = document->send(SCI_GETSELTEXT, 0, 0);
	std::string text(static_cast<std::size_t>(std::max<sptr_t>(length, 0)) + 1, '\0');
	document->send(SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>(text.data()));
	return g_strdup(text.c_str());
}

void hostReplaceSelection(void *context, const char *text)
{
	EditorDocument *document = static_cast<EditorWindow *>(context)->currentDocument();
	if (!document || !text)
		return;
	document->send(SCI_BEGINUNDOACTION);
	document->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(text));
	document->send(SCI_ENDUNDOACTION);
}

char *hostGetFilePath(void *context)
{
	EditorDocument *document = static_cast<EditorWindow *>(context)->currentDocument();
	return g_strdup(document ? document->path.c_str() : "");
}

void hostOpenFile(void *context, const char *path)
{
	if (path && *path)
		static_cast<EditorWindow *>(context)->openPath(path);
}

void hostNewDocument(void *context, const char *text)
{
	static_cast<EditorWindow *>(context)->createDocument(text ? text : "");
}

void hostShowMessage(void *context, const char *message)
{
	static_cast<EditorWindow *>(context)->showMessage(message ? message : "");
}

void hostSetStatus(void *context, const char *message)
{
	static_cast<EditorWindow *>(context)->setStatusMessage(message ? message : "");
}

void hostFreeText(char *text)
{
	g_free(text);
}

NppLinuxHost makePluginHost(EditorWindow *window)
{
	NppLinuxHost host{};
	host.apiVersion = NPP_LINUX_PLUGIN_API_VERSION;
	host.context = window;
	host.sendEditor = hostSendEditor;
	host.getText = hostGetText;
	host.getSelection = hostGetSelection;
	host.replaceSelection = hostReplaceSelection;
	host.getFilePath = hostGetFilePath;
	host.openFile = hostOpenFile;
	host.newDocument = hostNewDocument;
	host.showMessage = hostShowMessage;
	host.setStatus = hostSetStatus;
	host.freeText = hostFreeText;
	return host;
}

void pluginCommandActivated(GtkMenuItem *item, gpointer data)
{
	const auto plugin = static_cast<std::size_t>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "npp-plugin")));
	const auto command = static_cast<std::size_t>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "npp-plugin-command")));
	static_cast<EditorWindow *>(data)->runPluginCommand(plugin, command);
}

void mapNotified(ScintillaObject *, int, SCNotification *notification, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	if (notification->nmhdr.code == SCN_STYLENEEDED)
		window->styleNeeded(window->currentDocument(), notification->position);
}

std::string rgbaToHex(const GdkRGBA &colour)
{
	return colourToHex(rgb(static_cast<int>(colour.red * 255 + 0.5), static_cast<int>(colour.green * 255 + 0.5), static_cast<int>(colour.blue * 255 + 0.5)));
}

GdkRGBA rgbaFromColour(int colour)
{
	GdkRGBA rgba{};
	rgba.red = (colour & 0xFF) / 255.0;
	rgba.green = ((colour >> 8) & 0xFF) / 255.0;
	rgba.blue = ((colour >> 16) & 0xFF) / 255.0;
	rgba.alpha = 1.0;
	return rgba;
}

struct StyleDialog
{
	Theme theme;
	bool dark = false;
	GtkWidget *items = nullptr;
	GtkWidget *fore = nullptr;
	GtkWidget *back = nullptr;
	GtkWidget *bold = nullptr;
	GtkWidget *italic = nullptr;
	int current = 0;
	bool loading = false;
};

void styleDialogLoad(StyleDialog *dialog)
{
	dialog->loading = true;
	const auto item = static_cast<ThemeItem>(dialog->current);
	const StyleSpec &spec = dialog->theme[item];
	gtk_widget_set_sensitive(dialog->fore, themeItemHasForeground(item));
	gtk_widget_set_sensitive(dialog->back, themeItemHasBackground(item));
	gtk_widget_set_sensitive(dialog->bold, themeItemHasFontStyle(item));
	gtk_widget_set_sensitive(dialog->italic, themeItemHasFontStyle(item));
	const GdkRGBA fore = rgbaFromColour(spec.fore >= 0 ? spec.fore : dialog->theme[ThemeItem::Text].fore);
	const GdkRGBA back = rgbaFromColour(spec.back >= 0 ? spec.back : dialog->theme[ThemeItem::Text].back);
	gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog->fore), &fore);
	gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog->back), &back);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dialog->bold), spec.bold);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dialog->italic), spec.italic);
	dialog->loading = false;
}

void styleDialogStore(GtkWidget *, gpointer data)
{
	auto *dialog = static_cast<StyleDialog *>(data);
	if (dialog->loading)
		return;
	const auto item = static_cast<ThemeItem>(dialog->current);
	StyleSpec &spec = dialog->theme[item];
	GdkRGBA colour;
	if (themeItemHasForeground(item))
	{
		gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog->fore), &colour);
		spec.fore = colourFromHex(rgbaToHex(colour), spec.fore);
	}
	if (themeItemHasBackground(item))
	{
		gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog->back), &colour);
		spec.back = colourFromHex(rgbaToHex(colour), spec.back);
	}
	if (themeItemHasFontStyle(item))
	{
		spec.bold = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dialog->bold));
		spec.italic = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dialog->italic));
	}
}

void styleDialogItemChanged(GtkComboBox *combo, gpointer data)
{
	auto *dialog = static_cast<StyleDialog *>(data);
	dialog->current = std::max(0, gtk_combo_box_get_active(combo));
	styleDialogLoad(dialog);
}

struct LanguageDialog
{
	EditorWindow *window = nullptr;
	std::vector<UserLanguage> languages;
	GtkWidget *selector = nullptr;
	GtkWidget *name = nullptr;
	GtkWidget *extensions = nullptr;
	GtkWidget *lineComment = nullptr;
	GtkWidget *blockOpen = nullptr;
	GtkWidget *blockClose = nullptr;
	GtkWidget *keywords[4] = {};
	GtkWidget *operators = nullptr;
	GtkWidget *stringOpen = nullptr;
	GtkWidget *stringEscape = nullptr;
	GtkWidget *caseIgnored = nullptr;
};

void languageDialogLoad(GtkComboBox *combo, gpointer data)
{
	auto *dialog = static_cast<LanguageDialog *>(data);
	const int index = gtk_combo_box_get_active(combo);
	const UserLanguage empty;
	const UserLanguage &language = index > 0 && static_cast<std::size_t>(index) <= dialog->languages.size()
		? dialog->languages[static_cast<std::size_t>(index - 1)] : empty;
	gtk_entry_set_text(GTK_ENTRY(dialog->name), language.name.c_str());
	gtk_entry_set_text(GTK_ENTRY(dialog->extensions), joinWords(language.extensions).c_str());
	gtk_entry_set_text(GTK_ENTRY(dialog->lineComment), language.lineComments.empty() ? "" : language.lineComments.front().c_str());
	gtk_entry_set_text(GTK_ENTRY(dialog->blockOpen), language.blockOpen.c_str());
	gtk_entry_set_text(GTK_ENTRY(dialog->blockClose), language.blockClose.c_str());
	for (int i = 0; i < 4; ++i)
		gtk_entry_set_text(GTK_ENTRY(dialog->keywords[i]), joinWords(language.keywords[static_cast<std::size_t>(i)]).c_str());
	gtk_entry_set_text(GTK_ENTRY(dialog->operators), joinWords(language.operators).c_str());
	gtk_entry_set_text(GTK_ENTRY(dialog->stringOpen), language.delimiters[0].open.c_str());
	gtk_entry_set_text(GTK_ENTRY(dialog->stringEscape), language.delimiters[0].escape.c_str());
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dialog->caseIgnored), language.caseIgnored);
}

struct PluginRow
{
	EditorWindow *window;
	std::string id;
};

void pluginToggled(GtkToggleButton *button, gpointer data)
{
	auto *row = static_cast<PluginRow *>(data);
	row->window->enablePlugin(row->id, gtk_toggle_button_get_active(button));
}

GtkWidget *iconButton(const char *icon, const char *tooltip)
{
	GtkWidget *button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
	gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_set_focus_on_click(button, FALSE);
	return button;
}

std::string folded(const std::string &text)
{
	gchar *value = g_utf8_casefold(text.c_str(), -1);
	std::string result = value ? value : text;
	g_free(value);
	return result;
}

std::string plural(std::size_t count, const char *singular, const char *pluralForm)
{
	return std::to_string(count) + " " + (count == 1 ? singular : pluralForm);
}

std::string selectedLine(const EditorDocument *document)
{
	if (!document)
		return {};
	const sptr_t start = std::min(document->send(SCI_GETCURRENTPOS), document->send(SCI_GETANCHOR));
	const sptr_t end = std::max(document->send(SCI_GETCURRENTPOS), document->send(SCI_GETANCHOR));
	const sptr_t length = end - start;
	if (length <= 0 || length >= 200)
		return {};
	std::string selection(static_cast<std::size_t>(length) + 1, '\0');
	document->send(SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>(selection.data()));
	selection.resize(static_cast<std::size_t>(length));
	// A selection kept across replacements can end inside a multibyte character.
	if (!g_utf8_validate(selection.c_str(), static_cast<gssize>(selection.size()), nullptr))
		return {};
	return selection.find_first_of("\r\n") == std::string::npos ? selection : std::string();
}
}

EditorWindow::EditorWindow(bool selfTesting, bool noSession, bool recoveryOnly)
	: _selfTesting(selfTesting), _noSession(noSession), _settingsStore(selfTesting),
	_sessionStore(_settingsStore.path().parent_path())
{
	_settings = _settingsStore.load();
	_sessionWritable = _settings.restoreSession;
	if (selfTesting || noSession)
		_settings.restoreSession = recoveryOnly;
	_theme = parseTheme(_settings.darkTheme ? _settings.darkStyles : _settings.lightStyles, _settings.darkTheme);
	_pluginHost = makePluginHost(this);
	buildUi();
	buildContextMenus();
	reloadUserLanguages();
	if (!selfTesting)
		scanPlugins(PluginManager::defaultDirectories());
	loadSession();
	if (_documents.empty())
		createDocument();
	applySettings();
	_macro = _sessionStore.loadMacro();
	if (_noSession)
		_settings.restoreSession = true;
	updateMacroState();
	rebuildMacroMenu();
	restartBackupTimer();
	_monitorTimer = g_timeout_add_seconds(1, [](gpointer data) -> gboolean {
		static_cast<EditorWindow *>(data)->monitorTick();
		return G_SOURCE_CONTINUE;
	}, this);
}

EditorWindow::~EditorWindow()
{
	if (_findDialog)
		gtk_widget_destroy(_findDialog);
	if (_compareWindow)
		gtk_widget_destroy(_compareWindow);
	if (_functionTimer)
		g_source_remove(_functionTimer);
	if (_backupTimer)
		g_source_remove(_backupTimer);
	if (_monitorTimer)
		g_source_remove(_monitorTimer);
	stopInstanceServer();
	if (_recordingMacro)
		stopMacroRecording();
	saveSession();
	saveSettings();
	if (_window)
		gtk_widget_destroy(_window);
}

void EditorWindow::buildUi()
{
	_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(_window), "Notepad++ Linux");
	gtk_window_set_default_size(GTK_WINDOW(_window), 1100, 720);
	gtk_window_set_position(GTK_WINDOW(_window), GTK_WIN_POS_CENTER);
	g_signal_connect(_window, "delete-event", G_CALLBACK(windowDelete), this);
	// Coming back to the editor checks whether another program changed an open file.
	g_signal_connect(_window, "focus-in-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer data) -> gboolean {
		g_idle_add(+[](gpointer window) -> gboolean {
			static_cast<EditorWindow *>(window)->checkExternalChanges(false);
			return G_SOURCE_REMOVE;
		}, data);
		return FALSE;
	}), this);

	GtkWidget *mainBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(_window), mainBox);
	g_object_set_data(G_OBJECT(_window), "npp-menu-box", mainBox);
	buildMenu();
	buildToolbar();
	buildSearchBar();

	_notebook = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(_notebook), TRUE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(_notebook), FALSE);
	g_signal_connect(_notebook, "switch-page", G_CALLBACK(pageSwitched), this);
	g_signal_connect(_notebook, "page-reordered", G_CALLBACK(pageReordered), this);
	g_signal_connect_after(_notebook, "switch-page", G_CALLBACK(pageSwitchedAfter), this);
	_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	gtk_paned_pack1(GTK_PANED(_paned), _notebook, TRUE, FALSE);
	buildResultsPanel();
	gtk_paned_pack2(GTK_PANED(_paned), _resultsPanel, FALSE, TRUE);
	_innerPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(_innerPaned), _paned, TRUE, FALSE);
	buildSidebar();
	gtk_paned_pack2(GTK_PANED(_innerPaned), _sidebar, FALSE, FALSE);
	_outerPaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	buildWorkspacePanel();
	gtk_paned_pack1(GTK_PANED(_outerPaned), _workspacePanel, FALSE, FALSE);
	gtk_paned_pack2(GTK_PANED(_outerPaned), _innerPaned, TRUE, FALSE);
	gtk_box_pack_start(GTK_BOX(mainBox), _outerPaned, TRUE, TRUE, 0);
	gtk_widget_show(_notebook);
	gtk_widget_show(_paned);
	gtk_widget_show(_innerPaned);
	gtk_widget_show(_outerPaned);

	GtkWidget *statusBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_style_context_add_class(gtk_widget_get_style_context(statusBox), "npp-statusbar");
	gtk_widget_set_name(statusBox, "statusbar");
	gtk_container_set_border_width(GTK_CONTAINER(statusBox), 4);
	_positionLabel = gtk_label_new("Ln 1, Col 1");
	_languageLabel = gtk_label_new("Texto simples");
	_encodingLabel = gtk_label_new("UTF-8");
	_eolLabel = gtk_label_new("Unix (LF)");
	_messageLabel = gtk_label_new("Pronto");
	gtk_label_set_xalign(GTK_LABEL(_messageLabel), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(_messageLabel), PANGO_ELLIPSIZE_MIDDLE);
	gtk_box_pack_start(GTK_BOX(statusBox), _messageLabel, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(statusBox), _positionLabel, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(statusBox), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(statusBox), _languageLabel, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(statusBox), _encodingLabel, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(statusBox), _eolLabel, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(mainBox), statusBox, FALSE, FALSE, 0);
	_statusBar = statusBox;
}

void EditorWindow::buildMenu()
{
	GtkAccelGroup *accelerators = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(_window), accelerators);
	GtkWidget *bar = gtk_menu_bar_new();
	_menuBar = bar;
	GtkWidget *mainBox = GTK_WIDGET(g_object_get_data(G_OBJECT(_window), "npp-menu-box"));
	gtk_box_pack_start(GTK_BOX(mainBox), bar, FALSE, FALSE, 0);
	const auto none = static_cast<GdkModifierType>(0);
	const auto control = GDK_CONTROL_MASK;
	const auto shift = GDK_SHIFT_MASK;
	const auto alt = GDK_MOD1_MASK;
	const auto shiftControl = static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_SHIFT_MASK);
	const auto altControl = static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_MOD1_MASK);
	const auto altShift = static_cast<GdkModifierType>(GDK_MOD1_MASK | GDK_SHIFT_MASK);
	const auto altShiftControl = static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK);
	auto separator = [](GtkWidget *menu) { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); };
	auto item = [&](GtkWidget *menu, const char *label, Action action, guint key = 0, GdkModifierType modifiers = static_cast<GdkModifierType>(0)) {
		return menuItem(menu, label, this, action, accelerators, key, modifiers);
	};

	// Arquivo
	GtkWidget *file = gtk_menu_new();
	item(file, "_Novo", Action::NewFile, GDK_KEY_n, control);
	item(file, "_Abrir…", Action::OpenFile, GDK_KEY_o, control);
	GtkWidget *containing = gtk_menu_new();
	item(containing, "_Gerenciador de arquivos", Action::OpenContainingFolder);
	item(containing, "_Terminal", Action::OpenContainingTerminal);
	separator(containing);
	item(containing, "Pasta como _espaço de trabalho", Action::OpenFolderAsWorkspace);
	submenu(file, "Abrir pasta _contendo", containing);
	item(file, "Abrir no _visualizador padrão", Action::OpenInDefaultViewer);
	item(file, "Abrir pasta como espaço de _trabalho…", Action::OpenWorkspaceFolder);
	item(file, "Recarre_gar do disco", Action::ReloadFromDisk);
	item(file, "_Salvar", Action::SaveFile, GDK_KEY_s, control);
	item(file, "Salvar _como…", Action::SaveFileAs, GDK_KEY_s, altControl);
	item(file, "Salvar uma cóp_ia como…", Action::SaveCopyAs);
	item(file, "Salvar _tudo", Action::SaveAll, GDK_KEY_s, shiftControl);
	item(file, "Re_nomear…", Action::RenameFile);
	item(file, "_Fechar", Action::CloseFile, GDK_KEY_w, control);
	item(file, "Fechar t_udo", Action::CloseAll, GDK_KEY_w, shiftControl);
	GtkWidget *closeMany = gtk_menu_new();
	item(closeMany, "Fechar todos exceto o _atual", Action::CloseAllButActive);
	item(closeMany, "Fechar todos à _esquerda", Action::CloseAllToLeft);
	item(closeMany, "Fechar todos à _direita", Action::CloseAllToRight);
	item(closeMany, "Fechar todos os _inalterados", Action::CloseAllUnchanged);
	submenu(file, "Fechar _vários documentos", closeMany);
	item(file, "Restaurar arquivo fechado recentemente", Action::RestoreClosedFile, GDK_KEY_t, shiftControl);
	item(file, "Mover para a _lixeira", Action::MoveToTrash);
	separator(file);
	item(file, "Carregar s_essão…", Action::LoadSessionFile);
	item(file, "Salvar sessã_o…", Action::SaveSessionFile);
	item(file, "Abrir espaço de trabal_ho…", Action::OpenWorkspaceFile);
	item(file, "Salvar espaço de trabalho como…", Action::SaveWorkspaceFile);
	separator(file);
	item(file, "_Imprimir…", Action::Print, GDK_KEY_p, control);
	item(file, "Imprimir _agora", Action::PrintNow);
	separator(file);
	_recentMenu = gtk_menu_new();
	submenu(file, "Abrir _recente", _recentMenu);
	item(file, "Salvar _backup agora", Action::BackupNow);
	separator(file);
	item(file, "Sai_r", Action::Quit, GDK_KEY_q, control);
	submenu(bar, "_Arquivo", file);

	// Editar
	GtkWidget *edit = gtk_menu_new();
	item(edit, "_Desfazer", Action::Undo, GDK_KEY_z, control);
	item(edit, "_Refazer", Action::Redo, GDK_KEY_y, control);
	separator(edit);
	item(edit, "Recor_tar", Action::Cut, GDK_KEY_x, control);
	item(edit, "_Copiar", Action::Copy, GDK_KEY_c, control);
	item(edit, "Co_lar", Action::Paste, GDK_KEY_v, control);
	item(edit, "_Excluir", Action::DeleteSelection, GDK_KEY_Delete, none);
	item(edit, "Selecionar _tudo", Action::SelectAll, GDK_KEY_a, control);
	item(edit, "Iniciar/terminar _seleção", Action::BeginEndSelect);
	item(edit, "Iniciar/terminar seleção em coluna", Action::BeginEndSelectColumn);
	separator(edit);
	GtkWidget *insert = gtk_menu_new();
	item(insert, "Data e hora (_curta)", Action::InsertDateShort);
	item(insert, "Data e hora (_longa)", Action::InsertDateLong);
	item(insert, "Data e hora (_personalizada)", Action::InsertDateCustom);
	submenu(edit, "_Inserir", insert);
	GtkWidget *clipboard = gtk_menu_new();
	item(clipboard, "Copiar _caminho completo do arquivo", Action::CopyFullPath);
	item(clipboard, "Copiar _nome do arquivo", Action::CopyFileName);
	item(clipboard, "Copiar caminho da _pasta", Action::CopyDirectoryPath);
	separator(clipboard);
	item(clipboard, "Copiar nomes de _todos os arquivos", Action::CopyAllNames);
	item(clipboard, "Copiar caminhos de todos os _arquivos", Action::CopyAllPaths);
	submenu(edit, "Copiar para a área de _transferência", clipboard);
	GtkWidget *indent = gtk_menu_new();
	item(indent, "_Aumentar recuo da linha (Tab)", Action::IndentIncrease);
	item(indent, "_Diminuir recuo da linha (Shift+Tab)", Action::IndentDecrease);
	submenu(edit, "Rec_uo", indent);
	GtkWidget *convert = gtk_menu_new();
	item(convert, "_MAIÚSCULAS", Action::UpperCase, GDK_KEY_u, shiftControl);
	item(convert, "m_inúsculas", Action::LowerCase, GDK_KEY_u, control);
	item(convert, "_Primeiras Maiúsculas", Action::ProperCase, GDK_KEY_u, altControl);
	item(convert, "Primeiras Maiúsculas (_mantendo)", Action::ProperCaseBlend);
	item(convert, "_Frase com maiúscula", Action::SentenceCase, GDK_KEY_u, altShiftControl);
	item(convert, "Frase com maiúscula (m_antendo)", Action::SentenceCaseBlend);
	item(convert, "_iNVERTER mAIÚSCULAS", Action::InvertCase, GDK_KEY_u, altShift);
	item(convert, "_aLeAtÓrIo", Action::RandomCase);
	submenu(edit, "Con_verter letras", convert);
	GtkWidget *lines = gtk_menu_new();
	item(lines, "_Duplicar linha atual", Action::DuplicateLine, GDK_KEY_d, control);
	item(lines, "Excluir linha atual", Action::DeleteLine, GDK_KEY_l, shiftControl);
	item(lines, "Remover linhas d_uplicadas", Action::RemoveDuplicateLines);
	item(lines, "Remover duplicadas _consecutivas", Action::RemoveConsecutiveDuplicates);
	item(lines, "_Dividir linhas", Action::SplitLines, GDK_KEY_i, control);
	item(lines, "_Juntar linhas", Action::JoinLines, GDK_KEY_j, control);
	item(lines, "Mover linha para _cima", Action::MoveLineUp, GDK_KEY_Up, shiftControl);
	item(lines, "Mover linha para _baixo", Action::MoveLineDown, GDK_KEY_Down, shiftControl);
	item(lines, "Remover linhas _vazias", Action::RemoveEmptyLines);
	item(lines, "Remover linhas vazias (com e_spaços)", Action::RemoveBlankLines);
	item(lines, "Inserir linha em branco _acima", Action::InsertLineAbove, GDK_KEY_Return, altControl);
	item(lines, "Inserir linha em branco abai_xo", Action::InsertLineBelow, GDK_KEY_Return, altShiftControl);
	item(lines, "_Inverter ordem das linhas", Action::ReverseLines);
	item(lines, "_Embaralhar linhas", Action::RandomizeLines);
	separator(lines);
	item(lines, "Ordenar lexicograficamente (crescente)", Action::SortLexicalAsc);
	item(lines, "Ordenar ignorando maiúsculas (crescente)", Action::SortIgnoreCaseAsc);
	item(lines, "Ordenar pelo idioma (crescente)", Action::SortLocaleAsc);
	item(lines, "Ordenar como inteiros (crescente)", Action::SortIntegerAsc);
	item(lines, "Ordenar como decimais com vírgula (crescente)", Action::SortDecimalCommaAsc);
	item(lines, "Ordenar como decimais com ponto (crescente)", Action::SortDecimalDotAsc);
	item(lines, "Ordenar pelo comprimento (crescente)", Action::SortLengthAsc);
	separator(lines);
	item(lines, "Ordenar lexicograficamente (decrescente)", Action::SortLexicalDesc);
	item(lines, "Ordenar ignorando maiúsculas (decrescente)", Action::SortIgnoreCaseDesc);
	item(lines, "Ordenar pelo idioma (decrescente)", Action::SortLocaleDesc);
	item(lines, "Ordenar como inteiros (decrescente)", Action::SortIntegerDesc);
	item(lines, "Ordenar como decimais com vírgula (decrescente)", Action::SortDecimalCommaDesc);
	item(lines, "Ordenar como decimais com ponto (decrescente)", Action::SortDecimalDotDesc);
	item(lines, "Ordenar pelo comprimento (decrescente)", Action::SortLengthDesc);
	separator(lines);
	item(lines, "Ordenar de _A a Z (ignorando maiúsculas)", Action::SortLinesAscending);
	item(lines, "Ordenar de _Z a A (ignorando maiúsculas)", Action::SortLinesDescending);
	submenu(edit, "_Operações de linha", lines);
	GtkWidget *comment = gtk_menu_new();
	item(comment, "_Alternar comentário de linha", Action::ToggleComment, GDK_KEY_k, control);
	item(comment, "_Comentar linhas", Action::LineComment);
	item(comment, "_Descomentar linhas", Action::LineUncomment, GDK_KEY_k, shiftControl);
	item(comment, "Comentário em _bloco", Action::BlockComment, GDK_KEY_q, shiftControl);
	item(comment, "_Remover comentário em bloco", Action::BlockUncomment);
	submenu(edit, "Co_mentar/descomentar", comment);
	GtkWidget *completion = gtk_menu_new();
	item(completion, "Completar _palavra", Action::WordCompletion, GDK_KEY_Return, control);
	item(completion, "Completar _caminho", Action::PathCompletion, GDK_KEY_space, altControl);
	submenu(edit, "_Autocompletar", completion);
	GtkWidget *eol = gtk_menu_new();
	item(eol, "_Windows (CR LF)", Action::ConvertToWindowsEol);
	item(eol, "_Unix (LF)", Action::ConvertToUnixEol);
	item(eol, "_Macintosh (CR)", Action::ConvertToMacEol);
	submenu(edit, "Conversão de _fim de linha", eol);
	GtkWidget *blank = gtk_menu_new();
	item(blank, "Remover espaços no _fim", Action::TrimTrailingWhitespace);
	item(blank, "Remover espaços no _início", Action::TrimLeading);
	item(blank, "Remover espaços no início e no fim", Action::TrimBoth);
	item(blank, "Fim de linha para _espaço", Action::EolToSpace);
	item(blank, "Remover espaços e fins de linha", Action::TrimBothEolToSpace);
	separator(blank);
	item(blank, "_Tabulação para espaços", Action::TabsToSpaces);
	item(blank, "Espaços para tabulação (_todos)", Action::SpacesToTabsAll);
	item(blank, "Espaços para tabulação (_início)", Action::SpacesToTabsLeading);
	submenu(edit, "Operações com _espaços", blank);
	GtkWidget *onSelection = gtk_menu_new();
	item(onSelection, "_Abrir arquivo", Action::OpenSelectedFile);
	item(onSelection, "Abrir _pasta no gerenciador de arquivos", Action::OpenSelectedFolder);
	separator(onSelection);
	item(onSelection, "Pesquisar na _internet", Action::SearchOnInternet);
	submenu(edit, "Na _seleção", onSelection);
	separator(edit);
	GtkWidget *multiAll = gtk_menu_new();
	item(multiAll, "Ignorar maiúsculas e palavra inteira", Action::SelectAllMatches);
	item(multiAll, "Só diferenciar maiúsculas", Action::SelectAllMatchesCase);
	item(multiAll, "Só palavra inteira", Action::SelectAllMatchesWord);
	item(multiAll, "Maiúsculas e palavra inteira", Action::SelectAllMatchesCaseWord);
	submenu(edit, "Selecionar _todas as ocorrências", multiAll);
	GtkWidget *multiNext = gtk_menu_new();
	item(multiNext, "Ignorar maiúsculas e palavra inteira", Action::SelectNextMatch);
	item(multiNext, "Só diferenciar maiúsculas", Action::SelectNextMatchCase);
	item(multiNext, "Só palavra inteira", Action::SelectNextMatchWord);
	item(multiNext, "Maiúsculas e palavra inteira", Action::SelectNextMatchCaseWord);
	submenu(edit, "Adicionar _próxima ocorrência à seleção", multiNext);
	item(edit, "Desfazer última seleção adicionada", Action::UndoLatestMultiSelect);
	item(edit, "Pular atual e ir para a próxima", Action::SkipMultiSelect);
	separator(edit);
	item(edit, "Modo _coluna…", Action::ColumnModeInfo);
	item(edit, "Editor de co_lunas…", Action::ColumnEditor, GDK_KEY_c, alt);
	separator(edit);
	_readOnlyCheck = checkItem(edit, "Somente _leitura neste documento", this, Action::ToggleReadOnly, false, accelerators);
	item(edit, "Somente leitura em todos os documentos", Action::ReadOnlyAll);
	item(edit, "Liberar edição em todos os documentos", Action::ClearReadOnlyAll);
	submenu(bar, "_Editar", edit);

	// Pesquisar
	GtkWidget *search = gtk_menu_new();
	item(search, "_Localizar…", Action::Find, GDK_KEY_f, control);
	item(search, "Localizar em _arquivos…", Action::FindInFiles, GDK_KEY_f, shiftControl);
	item(search, "Localizar _próximo", Action::FindNext, GDK_KEY_F3, none);
	item(search, "Localizar a_nterior", Action::FindPrevious, GDK_KEY_F3, shift);
	item(search, "_Selecionar e localizar próximo", Action::SelectFindNext, GDK_KEY_F3, control);
	item(search, "Selecionar e localizar anterior", Action::SelectFindPrevious, GDK_KEY_F3, shiftControl);
	item(search, "Localizar (_volátil) próximo", Action::FindVolatileNext, GDK_KEY_F3, altControl);
	item(search, "Localizar (volátil) anterior", Action::FindVolatilePrevious, GDK_KEY_F3, altShiftControl);
	item(search, "S_ubstituir…", Action::Replace, GDK_KEY_h, control);
	item(search, "Busca _incremental", Action::IncrementalSearch, GDK_KEY_i, altControl);
	item(search, "Mar_car…", Action::MarkDialog, GDK_KEY_m, control);
	item(search, "Janela de _resultados da busca", Action::ToggleResults, GDK_KEY_F7, none);
	item(search, "Próximo resultado da busca", Action::NextResult, GDK_KEY_F4, none);
	item(search, "Resultado anterior da busca", Action::PreviousResult, GDK_KEY_F4, shift);
	item(search, "_Ir para…", Action::GoToLine, GDK_KEY_g, control);
	item(search, "Ir para a _chave correspondente", Action::GoToMatchingBrace, GDK_KEY_b, control);
	item(search, "Selecionar tudo entre { } [ ] ou ( )", Action::SelectBetweenBraces, GDK_KEY_b, altControl);
	separator(search);
	GtkWidget *history = gtk_menu_new();
	item(history, "Ir para a _próxima alteração", Action::NextChange, GDK_KEY_Page_Down, altControl);
	item(history, "Ir para a alteração _anterior", Action::PreviousChange, GDK_KEY_Page_Up, altControl);
	submenu(search, "Histórico de _alterações", history);
	separator(search);
	GtkWidget *styleAll = gtk_menu_new();
	const char *styleNames[] = {"Usando o 1º estilo", "Usando o 2º estilo", "Usando o 3º estilo", "Usando o 4º estilo", "Usando o 5º estilo"};
	for (int i = 0; i < 5; ++i)
		item(styleAll, styleNames[i], static_cast<Action>(static_cast<int>(Action::StyleToken1) + i));
	submenu(search, "_Destacar todas as ocorrências", styleAll);
	GtkWidget *styleOne = gtk_menu_new();
	for (int i = 0; i < 5; ++i)
		item(styleOne, styleNames[i], static_cast<Action>(static_cast<int>(Action::StyleOneToken1) + i));
	submenu(search, "Destacar _um token", styleOne);
	GtkWidget *clearStyle = gtk_menu_new();
	const char *clearNames[] = {"Limpar 1º estilo", "Limpar 2º estilo", "Limpar 3º estilo", "Limpar 4º estilo", "Limpar 5º estilo"};
	for (int i = 0; i < 5; ++i)
		item(clearStyle, clearNames[i], static_cast<Action>(static_cast<int>(Action::ClearStyle1) + i));
	item(clearStyle, "Limpar todos os estilos", Action::ClearAllStyles);
	submenu(search, "Limpar d_estaques", clearStyle);
	item(search, "Ir para o destaque _acima", Action::JumpStyleUp);
	item(search, "Ir para o destaque a_baixo", Action::JumpStyleDown);
	GtkWidget *copyStyled = gtk_menu_new();
	const char *copyNames[] = {"1º estilo", "2º estilo", "3º estilo", "4º estilo", "5º estilo"};
	for (int i = 0; i < 5; ++i)
		item(copyStyled, copyNames[i], static_cast<Action>(static_cast<int>(Action::CopyStyled1) + i));
	item(copyStyled, "Todos os estilos", Action::CopyStyledAll);
	submenu(search, "_Copiar texto destacado", copyStyled);
	separator(search);
	GtkWidget *bookmarks = gtk_menu_new();
	item(bookmarks, "_Alternar marcador", Action::ToggleBookmark, GDK_KEY_F2, control);
	item(bookmarks, "_Próximo marcador", Action::NextBookmark, GDK_KEY_F2, none);
	item(bookmarks, "Marcador _anterior", Action::PreviousBookmark, GDK_KEY_F2, shift);
	item(bookmarks, "_Limpar todos os marcadores", Action::ClearBookmarks);
	item(bookmarks, "_Recortar linhas marcadas", Action::CutBookmarkedLines);
	item(bookmarks, "_Copiar linhas marcadas", Action::CopyBookmarkedLines);
	item(bookmarks, "C_olar substituindo linhas marcadas", Action::PasteReplaceBookmarked);
	item(bookmarks, "R_emover linhas marcadas", Action::RemoveBookmarkedLines);
	item(bookmarks, "Remover linhas _não marcadas", Action::RemoveUnbookmarkedLines);
	item(bookmarks, "_Inverter marcadores", Action::InverseBookmarks);
	submenu(search, "_Marcadores", bookmarks);
	submenu(bar, "_Pesquisar", search);

	// Exibir
	GtkWidget *view = gtk_menu_new();
	_alwaysOnTopCheck = checkItem(view, "Sempre _visível", this, Action::AlwaysOnTop, false, accelerators);
	_fullScreenCheck = checkItem(view, "_Tela cheia", this, Action::FullScreen, false, accelerators, GDK_KEY_F11, none);
	_distractionFreeCheck = checkItem(view, "Modo sem _distrações", this, Action::DistractionFree, false, accelerators);
	separator(view);
	item(view, "Ver arquivo atual no _navegador", Action::OpenInBrowser);
	GtkWidget *symbols = gtk_menu_new();
	_whitespaceCheck = checkItem(symbols, "Mostrar _espaços e tabulações", this, Action::ToggleWhitespace, _settings.showWhitespace, accelerators);
	_eolCheck = checkItem(symbols, "Mostrar _fim de linha", this, Action::ToggleEndOfLine, _settings.showEndOfLine, accelerators);
	_allCharactersCheck = checkItem(symbols, "Mostrar _todos os caracteres", this, Action::ShowAllCharacters, _settings.showWhitespace && _settings.showEndOfLine, accelerators);
	separator(symbols);
	_indentGuidesCheck = checkItem(symbols, "Mostrar guias de _recuo", this, Action::ToggleIndentGuides, _settings.indentGuides, accelerators);
	_wrapSymbolCheck = checkItem(symbols, "Mostrar símbolo de _quebra", this, Action::ToggleWrapSymbol, _settings.showWrapSymbol, accelerators);
	submenu(view, "Mostrar _símbolos", symbols);
	GtkWidget *zoom = gtk_menu_new();
	item(zoom, "_Aumentar (Ctrl+roda do mouse)", Action::ZoomIn, GDK_KEY_plus, control);
	item(zoom, "_Diminuir (Ctrl+roda do mouse)", Action::ZoomOut, GDK_KEY_minus, control);
	item(zoom, "_Restaurar zoom padrão", Action::ZoomReset, GDK_KEY_0, control);
	submenu(view, "_Zoom", zoom);
	GtkWidget *tabs = gtk_menu_new();
	for (int i = 0; i < 9; ++i)
	{
		const std::string label = std::to_string(i + 1) + "ª aba";
		item(tabs, label.c_str(), static_cast<Action>(static_cast<int>(Action::Tab1) + i));
	}
	separator(tabs);
	item(tabs, "Primeira aba", Action::FirstTab);
	item(tabs, "Última aba", Action::LastTab);
	item(tabs, "_Próxima aba", Action::NextTab, GDK_KEY_Page_Down, control);
	item(tabs, "Aba _anterior", Action::PreviousTab, GDK_KEY_Page_Up, control);
	separator(tabs);
	item(tabs, "Mover para o início", Action::MoveTabStart);
	item(tabs, "Mover para o fim", Action::MoveTabEnd);
	item(tabs, "Mover aba para a frente", Action::MoveTabForward, GDK_KEY_Page_Down, shiftControl);
	item(tabs, "Mover aba para trás", Action::MoveTabBackward, GDK_KEY_Page_Up, shiftControl);
	submenu(view, "A_ba", tabs);
	_wrapCheck = checkItem(view, "_Quebra automática de linha", this, Action::ToggleWordWrap, _settings.wordWrap, accelerators, GDK_KEY_w, altControl);
	_lineNumbersCheck = checkItem(view, "Mostrar _números das linhas", this, Action::ToggleLineNumbers, _settings.lineNumbers, accelerators);
	item(view, "_Ocultar linhas", Action::HideLines, GDK_KEY_h, alt);
	item(view, "Mostrar linhas ocultas", Action::ShowHiddenLines);
	separator(view);
	item(view, "Recolher tudo", Action::FoldAll, GDK_KEY_0, alt);
	item(view, "Expandir tudo", Action::UnfoldAll, GDK_KEY_0, altShift);
	item(view, "Recolher nível atual", Action::FoldCurrent, GDK_KEY_f, altControl);
	item(view, "Expandir nível atual", Action::UnfoldCurrent, GDK_KEY_f, altShiftControl);
	GtkWidget *foldLevels = gtk_menu_new();
	GtkWidget *unfoldLevels = gtk_menu_new();
	for (int i = 0; i < 8; ++i)
	{
		const std::string label = std::to_string(i + 1);
		item(foldLevels, label.c_str(), static_cast<Action>(static_cast<int>(Action::FoldLevel1) + i), GDK_KEY_1 + static_cast<guint>(i), alt);
		item(unfoldLevels, label.c_str(), static_cast<Action>(static_cast<int>(Action::UnfoldLevel1) + i), GDK_KEY_1 + static_cast<guint>(i), altShift);
	}
	submenu(view, "Recolher n_ível", foldLevels);
	submenu(view, "Expandir ní_vel", unfoldLevels);
	separator(view);
	item(view, "_Resumo…", Action::Summary);
	_workspaceCheck = checkItem(view, "Pasta como espaço de tra_balho", this, Action::ToggleWorkspace, _settings.showWorkspace, accelerators);
	_documentMapCheck = checkItem(view, "_Mapa do documento", this, Action::ToggleDocumentMap, _settings.showDocumentMap, accelerators);
	_functionListCheck = checkItem(view, "_Lista de funções", this, Action::ToggleFunctionList, _settings.showFunctionList, accelerators);
	separator(view);
	_monitoringCheck = checkItem(view, "Monitorar arquivo (tail -_f)", this, Action::Monitoring, false, accelerators);
	_darkCheck = checkItem(view, "Tema _escuro", this, Action::ToggleDarkTheme, _settings.darkTheme, accelerators);
	submenu(bar, "E_xibir", view);

	// Codificação
	_encodingMenu = gtk_menu_new();
	buildEncodingMenu(_encodingMenu);
	submenu(bar, "C_odificação", _encodingMenu);

	// Linguagem (the language list is filled by rebuildLanguageMenu)
	_languageMenu = gtk_menu_new();
	submenu(bar, "_Linguagem", _languageMenu);

	// Configurações
	GtkWidget *settings = gtk_menu_new();
	item(settings, "_Preferências…", Action::ShowPreferences);
	item(settings, "_Configurador de estilos…", Action::StyleConfigurator);
	item(settings, "_Atalhos de teclado…", Action::ShortcutList);
	GtkWidget *import = gtk_menu_new();
	item(import, "Importar _plugin…", Action::ImportPlugin);
	item(import, "Importar _linguagem do usuário…", Action::ImportUserLanguage);
	submenu(settings, "_Importar", import);
	submenu(bar, "Configuraçõe_s", settings);

	// Ferramentas
	GtkWidget *tools = gtk_menu_new();
	const char *hashLabels[] = {"MD5", "SHA-1", "SHA-256", "SHA-512"};
	for (int kind = 0; kind < 4; ++kind)
	{
		GtkWidget *hash = gtk_menu_new();
		const int base = static_cast<int>(Action::HashMd5Text) + kind * 3;
		item(hash, "_Gerar…", static_cast<Action>(base));
		item(hash, "Gerar a partir de _arquivos…", static_cast<Action>(base + 1));
		item(hash, "Gerar da _seleção para a área de transferência", static_cast<Action>(base + 2));
		submenu(tools, hashLabels[kind], hash);
	}
	submenu(bar, "Ferramen_tas", tools);

	// Macro (saved macros are added by rebuildMacroMenu)
	_macroMenu = gtk_menu_new();
	_recordMenuItem = item(_macroMenu, "_Iniciar gravação", Action::StartMacro);
	_stopMacroMenuItem = item(_macroMenu, "_Parar gravação", Action::StopMacro);
	item(_macroMenu, "_Alternar gravação", Action::ToggleMacroRecord, GDK_KEY_r, shiftControl);
	_playMacroMenuItem = item(_macroMenu, "_Reproduzir", Action::PlayMacro, GDK_KEY_p, shiftControl);
	item(_macroMenu, "_Salvar macro gravada…", Action::SaveMacroAs);
	_repeatMacroMenuItem = item(_macroMenu, "_Executar macro várias vezes…", Action::RunMacroMultiple);
	_clearMacroMenuItem = item(_macroMenu, "_Limpar macro gravada", Action::ClearMacro);
	submenu(bar, "_Macro", _macroMenu);

	// Executar
	GtkWidget *run = gtk_menu_new();
	item(run, "_Executar…", Action::RunCommand, GDK_KEY_F5, none);
	separator(run);
	item(run, "Abrir arquivo atual no _navegador", Action::OpenInBrowser);
	item(run, "Pesquisar seleção na _internet", Action::SearchOnInternet);
	item(run, "Abrir _terminal na pasta do arquivo", Action::OpenContainingTerminal);
	submenu(bar, "Exec_utar", run);

	_pluginMenu = gtk_menu_new();
	submenu(bar, "Plu_gins", _pluginMenu);
	g_object_set_data(G_OBJECT(_window), "npp-accelerators", accelerators);

	// Janela (open documents are listed by rebuildWindowMenu when it opens)
	_windowMenu = gtk_menu_new();
	g_signal_connect_swapped(_windowMenu, "show", G_CALLBACK(+[](gpointer data) { static_cast<EditorWindow *>(data)->rebuildWindowMenu(); }), this);
	submenu(bar, "_Janela", _windowMenu);
	rebuildWindowMenu();

	GtkWidget *help = gtk_menu_new();
	item(help, "_Argumentos de linha de comando…", Action::CommandLineHelp);
	item(help, "_Informações de depuração…", Action::DebugInfo);
	item(help, "_Status do port", Action::ShowRoadmap);
	separator(help);
	item(help, "_Sobre", Action::ShowAbout, GDK_KEY_F1, none);
	submenu(bar, "Aju_da", help);

	rebuildRecentMenu();
}

void EditorWindow::buildToolbar()
{
	GtkWidget *toolbar = gtk_toolbar_new();
	gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_BOTH_HORIZ);
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "npp-toolbar");
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_NEW, "Novo", "Novo arquivo (Ctrl+N)", this, Action::NewFile), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_OPEN, "Abrir", "Abrir arquivos (Ctrl+O)", this, Action::OpenFile), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_SAVE, "Salvar", "Salvar (Ctrl+S)", this, Action::SaveFile), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_UNDO, "Desfazer", "Desfazer (Ctrl+Z)", this, Action::Undo), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_REDO, "Refazer", "Refazer (Ctrl+Y)", this, Action::Redo), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_CUT, "Recortar", "Recortar (Ctrl+X)", this, Action::Cut), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_COPY, "Copiar", "Copiar (Ctrl+C)", this, Action::Copy), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_PASTE, "Colar", "Colar (Ctrl+V)", this, Action::Paste), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), gtk_separator_tool_item_new(), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_FIND, "Localizar", "Localizar e substituir (Ctrl+F)", this, Action::Find), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), toolButton(GTK_STOCK_FIND_AND_REPLACE, "Em arquivos", "Localizar ou substituir em arquivos e pastas (Ctrl+Shift+F)", this, Action::FindInFiles), -1);
	GtkWidget *mainBox = GTK_WIDGET(g_object_get_data(G_OBJECT(_window), "npp-menu-box"));
	gtk_box_pack_start(GTK_BOX(mainBox), toolbar, FALSE, FALSE, 0);
	_toolbar = toolbar;
}

void EditorWindow::buildSearchBar()
{
	_searchBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(_searchBar), 6);
	gtk_style_context_add_class(gtk_widget_get_style_context(_searchBar), "npp-searchbar");
	GtkBox *box = GTK_BOX(_searchBar);
	_searchEntry = labelledEntry(box, "Localizar:", "Texto ou expressão", 24);
	_replaceLabel = gtk_label_new("Substituir:");
	_replaceEntry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(_replaceEntry), "Novo texto");
	gtk_entry_set_width_chars(GTK_ENTRY(_replaceEntry), 18);
	gtk_box_pack_start(box, _replaceLabel, FALSE, FALSE, 0);
	gtk_box_pack_start(box, _replaceEntry, TRUE, TRUE, 0);
	GtkWidget *next = gtk_button_new_with_mnemonic("_Próxima");
	GtkWidget *previous = gtk_button_new_with_mnemonic("_Anterior");
	_replaceButton = gtk_button_new_with_mnemonic("S_ubstituir");
	_replaceAllButton = gtk_button_new_with_mnemonic("Substituir _todas");
	gtk_box_pack_start(box, previous, FALSE, FALSE, 0);
	gtk_box_pack_start(box, next, FALSE, FALSE, 0);
	gtk_box_pack_start(box, _replaceButton, FALSE, FALSE, 0);
	gtk_box_pack_start(box, _replaceAllButton, FALSE, FALSE, 0);
	_matchCase = gtk_check_button_new_with_mnemonic("_Maiúsculas");
	_wholeWord = gtk_check_button_new_with_mnemonic("Palavra _inteira");
	_regularExpression = gtk_check_button_new_with_mnemonic("_Regex");
	gtk_widget_set_tooltip_text(_regularExpression, "Expressões regulares do Scintilla: use \\(grupo\\) e \\1 na substituição");
	_wrapAround = gtk_check_button_new_with_mnemonic("_Circular");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_wrapAround), TRUE);
	gtk_box_pack_start(box, _matchCase, FALSE, FALSE, 0);
	gtk_box_pack_start(box, _wholeWord, FALSE, FALSE, 0);
	gtk_box_pack_start(box, _regularExpression, FALSE, FALSE, 0);
	gtk_box_pack_start(box, _wrapAround, FALSE, FALSE, 0);
	GtkWidget *close = gtk_button_new_from_stock(GTK_STOCK_CLOSE);
	gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
	gtk_box_pack_end(box, close, FALSE, FALSE, 0);

	attachData(_window, "npp-search-entry", _searchEntry);
	attachData(_window, "npp-replace-entry", _replaceEntry);
	attachData(_window, "npp-match-case", _matchCase);
	attachData(_window, "npp-whole-word", _wholeWord);
	attachData(_window, "npp-regex", _regularExpression);
	attachData(_window, "npp-wrap-search", _wrapAround);
	attachData(_window, "npp-message", _messageLabel);
	attachData(_window, "npp-search-bar", _searchBar);
	attachData(_window, "npp-main-window", _window);
	attachData(_window, "npp-window", this);
	attachData(next, "npp-search-operation", GINT_TO_POINTER(0));
	attachData(previous, "npp-search-operation", GINT_TO_POINTER(1));
	attachData(_replaceButton, "npp-search-operation", GINT_TO_POINTER(2));
	attachData(_replaceAllButton, "npp-search-operation", GINT_TO_POINTER(3));
	g_signal_connect(next, "clicked", G_CALLBACK(searchActivated), this);
	g_signal_connect(_searchEntry, "activate", G_CALLBACK(searchActivated), this);
	g_signal_connect(previous, "clicked", G_CALLBACK(searchActivated), this);
	g_signal_connect(_replaceButton, "clicked", G_CALLBACK(searchActivated), this);
	g_signal_connect(_replaceAllButton, "clicked", G_CALLBACK(searchActivated), this);
	g_signal_connect(_replaceAllButton, "clicked", G_CALLBACK(searchActivated), this);
	g_signal_connect(close, "clicked", G_CALLBACK(hideSearch), _window);
	GtkWidget *mainBox = GTK_WIDGET(g_object_get_data(G_OBJECT(_window), "npp-menu-box"));
	gtk_box_pack_start(GTK_BOX(mainBox), _searchBar, FALSE, FALSE, 0);
}

EditorDocument *EditorWindow::createDocument(const std::string &text)
{
	GtkWidget *widget = scintilla_new();
	auto document = std::make_unique<EditorDocument>(widget);
	EditorDocument *result = document.get();
	_documents.push_back(std::move(document));
	++_untitledCounter;
	result->displayName = "Sem título " + std::to_string(_untitledCounter);
	attachData(widget, "npp-document", result);
	attachData(widget, "npp-window", this);
	attachData(widget, "npp-notebook", _notebook);
	g_signal_connect(widget, SCINTILLA_NOTIFY, G_CALLBACK(notificationReceived), this);

	GtkWidget *tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
	GtkWidget *label = gtk_label_new(result->displayName.c_str());
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), TabNameLimit);
	GtkWidget *close = gtk_button_new_with_label("×");
	gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(close, "Fechar arquivo");
	gtk_widget_set_focus_on_click(close, FALSE);
	g_signal_connect(close, "clicked", G_CALLBACK(closeTabClicked), result);
	gtk_box_pack_start(GTK_BOX(tab), label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(tab), close, FALSE, FALSE, 0);
	attachData(widget, "npp-tab-label", label);
	attachData(widget, "npp-tab", tab);
	attachData(widget, "npp-page-index", GINT_TO_POINTER(static_cast<gint>(_documents.size() - 1)));
	// The event box receives right and middle clicks on the tab.
	GtkWidget *tabArea = gtk_event_box_new();
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(tabArea), FALSE);
	gtk_container_add(GTK_CONTAINER(tabArea), tab);
	gtk_notebook_append_page(GTK_NOTEBOOK(_notebook), widget, tabArea);
	attachDocumentEvents(result, tabArea);
	attachData(widget, "npp-document", result);
	attachData(widget, "npp-window", this);
	attachData(widget, "npp-notebook", _notebook);
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(_notebook), widget, TRUE);
	gtk_widget_show_all(tabArea);
	gtk_widget_show(widget);
	result->setText(text);
	result->send(SCI_SETMULTIPLESELECTION, 1);
	result->send(SCI_SETADDITIONALSELECTIONTYPING, 1);
	result->send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
	result->send(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);
	result->send(SCI_SETMOUSESELECTIONRECTANGULARSWITCH, 1);
	if (_settings.changeHistory)
		result->send(SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS);
	applyTheme(*result);
	applyViewOptions(*result);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(_notebook), static_cast<int>(_documents.size() - 1));
	refreshDocument(result);
	return result;
}

void EditorWindow::reorderDocument(gint from, gint to)
{
	if (from < 0 || to < 0 || from == to
		|| static_cast<std::size_t>(from) >= _documents.size()
		|| static_cast<std::size_t>(to) >= _documents.size())
		return;
	auto document = std::move(_documents[static_cast<std::size_t>(from)]);
	_documents.erase(_documents.begin() + from);
	_documents.insert(_documents.begin() + to, std::move(document));
}

void EditorWindow::openPath(const std::filesystem::path &path)
{
	const auto absolute = std::filesystem::absolute(path).lexically_normal();
	for (std::size_t i = 0; i < _documents.size(); ++i)
	{
		if (!_documents[i]->path.empty() && _documents[i]->path == absolute)
		{
			gtk_notebook_set_current_page(GTK_NOTEBOOK(_notebook), static_cast<int>(i));
			gtk_widget_grab_focus(_documents[i]->widget());
			return;
		}
	}
	auto *document = createDocument();
	std::string error;
	if (!document->load(absolute, error))
	{
		const int index = gtk_notebook_page_num(GTK_NOTEBOOK(_notebook), document->widget());
		if (index >= 0)
			closeDocument(static_cast<std::size_t>(index), false);
		showError("Não foi possível abrir o arquivo.\n\n" + error);
		return;
	}
	document->language = &languageFor(absolute);
	applyTheme(*document);
	addRecentFile(absolute);
	refreshDocument(document);
}

bool EditorWindow::saveDocument(EditorDocument &document, bool askPath)
{
	if (askPath || document.path.empty())
	{
		if (_selfTesting && _selfTestDirectory.empty())
			return false;
		GtkWidget *dialog = gtk_file_chooser_dialog_new("Salvar arquivo", GTK_WINDOW(_window), GTK_FILE_CHOOSER_ACTION_SAVE,
			"_Cancelar", GTK_RESPONSE_CANCEL, "_Salvar", GTK_RESPONSE_ACCEPT, nullptr);
		gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
		if (!document.path.empty())
			gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), document.path.c_str());
		else
			gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), document.displayName.c_str());
		const bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
		char *filename = accepted ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)) : nullptr;
		gtk_widget_destroy(dialog);
		if (!filename)
			return false;
		const std::filesystem::path path = filename;
		g_free(filename);
		document.path = path;
	}
	std::string error;
	if (!document.save(document.path, error))
	{
		showError("Não foi possível salvar o arquivo.\n\n" + error);
		return false;
	}
	document.language = &languageFor(document.path);
	applyTheme(document);
	_sessionStore.discardBackup(document);
	document.recoveryId.clear();
	if (document.send(SCI_GETMODIFY))
	{
		document.recoveryId = std::to_string(static_cast<long long>(g_get_real_time())) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&document));
		_sessionStore.backup(document);
	}
	saveSession();
	addRecentFile(document.path);
	refreshDocument(&document);
	return true;
}

bool EditorWindow::confirmDiscard(EditorDocument &document)
{
	if (!document.send(SCI_GETMODIFY))
		return true;
	if (_selfTesting)
		return true;
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "Salvar as alterações em “%s”?", document.displayName.c_str());
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "Se você fechar sem salvar, as alterações serão perdidas.");
	gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Cancelar", GTK_RESPONSE_CANCEL,
		"Fechar _sem salvar", GTK_RESPONSE_NO, "_Salvar", GTK_RESPONSE_YES, nullptr);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
	const int response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	if (response == GTK_RESPONSE_YES)
		return saveDocument(document);
	return response == GTK_RESPONSE_NO;
}

bool EditorWindow::closeDocument(std::size_t index, bool askToSave, bool discardBackup)
{
	if (index >= _documents.size())
		return true;
	auto *document = _documents[index].get();
	if (askToSave && !confirmDiscard(*document))
		return false;
	if (discardBackup)
		_sessionStore.discardBackup(*document);
	if (!document->path.empty())
	{
		_closedFiles.erase(std::remove(_closedFiles.begin(), _closedFiles.end(), document->path.string()), _closedFiles.end());
		_closedFiles.push_back(document->path.string());
		if (_closedFiles.size() > 30)
			_closedFiles.erase(_closedFiles.begin());
	}
	compareDocumentClosed(document);
	_updating = true;
	gtk_notebook_remove_page(GTK_NOTEBOOK(_notebook), static_cast<int>(index));
	_documents.erase(_documents.begin() + static_cast<std::ptrdiff_t>(index));
	for (std::size_t i = index; i < _documents.size(); ++i)
		attachData(_documents[i]->widget(), "npp-page-index", GINT_TO_POINTER(static_cast<gint>(i)));
	_updating = false;
	if (_documents.empty() && !_selfTesting)
		createDocument();
	refreshDocument();
	saveSession();
	return true;
}

void EditorWindow::setCurrentPage(int page)
{
	gtk_notebook_set_current_page(GTK_NOTEBOOK(_notebook), page);
}

EditorDocument *EditorWindow::currentDocument() const
{
	const int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(_notebook));
	if (page < 0)
		return nullptr;
	GtkWidget *widget = gtk_notebook_get_nth_page(GTK_NOTEBOOK(_notebook), page);
	return widget ? static_cast<EditorDocument *>(g_object_get_data(G_OBJECT(widget), "npp-document")) : nullptr;
}

std::size_t EditorWindow::currentIndex() const
{
	const int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(_notebook));
	return page < 0 ? 0 : static_cast<std::size_t>(page);
}

std::size_t EditorWindow::documentCount() const noexcept
{
	return _documents.size();
}

void EditorWindow::updateTab(EditorDocument &document)
{
	auto *label = GTK_LABEL(g_object_get_data(G_OBJECT(document.widget()), "npp-tab-label"));
	if (!label)
		return;
	document.dirty = document.send(SCI_GETMODIFY) != 0;
	const std::string name = (document.dirty ? "● " : "") + document.displayName;
	gtk_label_set_text(label, name.c_str());
	// As in the Windows version the tab shows the whole name; very long names
	// stop at a limit and are shortened in the middle.
	const glong characters = g_utf8_strlen(name.c_str(), -1);
	gtk_label_set_width_chars(label, static_cast<gint>(std::min<glong>(characters, TabNameLimit)));
	GtkWidget *tab = GTK_WIDGET(g_object_get_data(G_OBJECT(document.widget()), "npp-tab"));
	gtk_widget_set_tooltip_text(tab, document.path.empty() ? "Arquivo ainda não salvo" : document.path.c_str());
}

void EditorWindow::refreshDocument(EditorDocument *document)
{
	if (_updating)
		return;
	if (!document)
		document = currentDocument();
	if (!document)
		return;
	updateTab(*document);
	if (document == currentDocument())
	{
		updateTitle();
		updateStatus();
		updateLanguageMenu();
		syncDocumentMap();
		updateBraceMatch(*document);
	}
}

void EditorWindow::updateTitle()
{
	const auto *document = currentDocument();
	const std::string title = document ? (document->dirty ? "*" : "") + document->displayName + " — Notepad++ Linux" : "Notepad++ Linux";
	gtk_window_set_title(GTK_WINDOW(_window), title.c_str());
}

void EditorWindow::updateStatus()
{
	const auto *document = currentDocument();
	if (!document)
		return;
	const sptr_t position = document->send(SCI_GETCURRENTPOS);
	const long line = static_cast<long>(document->send(SCI_LINEFROMPOSITION, position)) + 1;
	const long column = static_cast<long>(document->send(SCI_GETCOLUMN, position)) + 1;
	const std::string positionText = "Ln " + std::to_string(line) + ", Col " + std::to_string(column);
	gtk_label_set_text(GTK_LABEL(_positionLabel), positionText.c_str());
	gtk_label_set_text(GTK_LABEL(_languageLabel), document->language->name.c_str());
	gtk_label_set_text(GTK_LABEL(_encodingLabel), encodingLabel(document->encoding).c_str());
	updateEncodingMenu();
	if (_readOnlyCheck)
	{
		const bool wasUpdating = _updating;
		_updating = true;
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_readOnlyCheck), document->send(SCI_GETREADONLY) != 0);
		if (_monitoringCheck)
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_monitoringCheck), document->monitoring);
		_updating = wasUpdating;
	}
	gtk_label_set_text(GTK_LABEL(_eolLabel), eolName(document->eolMode));
	if (_recordingMacro)
		gtk_label_set_text(GTK_LABEL(_messageLabel), "Gravando macro…");
}

void EditorWindow::updateLanguageMenu()
{
	const auto *document = currentDocument();
	if (!document)
		return;
	if (GtkWidget *item = GTK_WIDGET(g_object_get_data(G_OBJECT(_languageMenu), "npp-current-language")))
	{
		_updating = true;
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), FALSE);
		_updating = false;
	}
	GList *children = gtk_container_get_children(GTK_CONTAINER(_languageMenu));
	_updating = true;
	for (GList *entry = children; entry; entry = entry->next)
	{
		GtkWidget *item = GTK_WIDGET(entry->data);
		const auto *language = static_cast<const Language *>(g_object_get_data(G_OBJECT(item), "npp-language"));
		if (language)
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), language->id == document->language->id);
	}
	_updating = false;
	g_list_free(children);
}

void EditorWindow::applyTheme(EditorDocument &document)
{
	applyStyles(document, *document.language);
	Scintilla::ILexer5 *lexer = document.language->lexer.empty() ? nullptr : CreateLexer(document.language->lexer.c_str());
	document.send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(lexer));
	for (const char *property : {"fold", "fold.compact", "fold.comment", "fold.html", "fold.preprocessor"})
		document.send(SCI_SETPROPERTY, reinterpret_cast<uptr_t>(property), reinterpret_cast<sptr_t>(std::string(property) == "fold.compact" ? "0" : "1"));
	applyMargins(document);
	const NppLanguageSpec spec = nppSpecFor(document);
	if (!spec.keywordSets.empty())
	{
		// The keyword lists of the Windows version, in the slots its lexers expect.
		const NppLexerStyles *styles = nppLexerStyles(spec.styler, _settings.darkTheme);
		for (const auto &[language, set] : spec.keywordSets)
		{
			std::string words = nppKeywords(language, set.first);
			if (styles && language == spec.language)
			{
				for (const auto &style : styles->styles)
				{
					if (style.keywordClass == set.first && !style.extraKeywords.empty())
						words += " " + style.extraKeywords;
				}
			}
			document.sendString(SCI_SETKEYWORDS, static_cast<uptr_t>(set.second), words);
		}
		for (const auto &[name, value] : spec.properties)
			document.send(SCI_SETPROPERTY, reinterpret_cast<uptr_t>(name.c_str()), reinterpret_cast<sptr_t>(value.c_str()));
	}
	else if (!document.language->keywords.empty())
		document.sendString(SCI_SETKEYWORDS, 0, document.language->keywords);
	document.send(SCI_COLOURISE, 0, -1);
}

NppLanguageSpec EditorWindow::nppSpecFor(const EditorDocument &document) const
{
	if (!_settings.nppLanguageColors || userLanguageFor(document.language))
		return {};
	const std::string name = document.path.empty() ? document.displayName : document.path.filename().string();
	NppLanguageSpec spec = nppLanguageSpec(document.language->id, name);
	if (!spec.styler.empty() && !nppLexerStyles(spec.styler, _settings.darkTheme))
		return {};
	return spec;
}

void EditorWindow::applyStyles(EditorDocument &document, const Language &language, const EditorDocument *source)
{
	const Theme &theme = _theme;
	const StyleSpec &text = theme[ThemeItem::Text];
	auto apply = [&](int id, const StyleSpec &spec) {
		if (spec.fore >= 0)
			document.send(SCI_STYLESETFORE, id, spec.fore);
		document.send(SCI_STYLESETBOLD, id, spec.bold ? 1 : 0);
		document.send(SCI_STYLESETITALIC, id, spec.italic ? 1 : 0);
	};
	document.send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(_settings.fontFamily.c_str()));
	document.send(SCI_STYLESETSIZE, STYLE_DEFAULT, _settings.fontSize);
	document.send(SCI_STYLESETFORE, STYLE_DEFAULT, text.fore);
	document.send(SCI_STYLESETBACK, STYLE_DEFAULT, text.back);
	document.send(SCI_STYLESETBOLD, STYLE_DEFAULT, text.bold ? 1 : 0);
	document.send(SCI_STYLESETITALIC, STYLE_DEFAULT, text.italic ? 1 : 0);
	document.send(SCI_STYLECLEARALL);
	const StyleSpec &lineNumber = theme[ThemeItem::LineNumber];
	document.send(SCI_STYLESETFORE, STYLE_LINENUMBER, lineNumber.fore);
	document.send(SCI_STYLESETBACK, STYLE_LINENUMBER, lineNumber.back);
	document.send(SCI_STYLESETFORE, STYLE_INDENTGUIDE, lineNumber.fore);
	apply(STYLE_BRACELIGHT, theme[ThemeItem::BraceMatch]);
	style(document, STYLE_BRACEBAD, colour(220, 60, 60), true);
	document.send(SCI_SETCARETFORE, text.fore);
	// Setting the colour re-enables the highlight, so visibility is applied last.
	document.send(SCI_SETCARETLINEBACK, theme[ThemeItem::CaretLine].back);
	document.send(SCI_SETCARETLINEBACKALPHA, 256);
	document.send(SCI_SETCARETLINEVISIBLE, _settings.highlightCaretLine ? 1 : 0);
	document.send(SCI_SETSELFORE, 1, theme[ThemeItem::Selection].fore);
	document.send(SCI_SETSELBACK, 1, theme[ThemeItem::Selection].back);
	document.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
	document.send(SCI_SETMARGINWIDTHN, 0, _settings.lineNumbers ? 52 : 0);
	document.send(SCI_SETWHITESPACEFORE, 1, lineNumber.fore);
	document.send(SCI_SETWHITESPACEBACK, 1, text.back);
	document.send(SCI_SETEDGECOLOUR, lineNumber.back);
	const EditorDocument &owner = source ? *source : document;
	const NppLanguageSpec spec = owner.language == &language ? nppSpecFor(owner) : NppLanguageSpec{};
	if (const NppLexerStyles *styles = spec.styler.empty() ? nullptr : nppLexerStyles(spec.styler, _settings.darkTheme))
	{
		// Same colours as the Windows version for this language. The background
		// stays the theme's, except where the Windows style has its own.
		for (const auto &style : styles->styles)
		{
			if (style.id < 0 || style.id >= 128 || (style.id >= STYLE_DEFAULT && style.id <= STYLE_LASTPREDEFINED))
				continue;
			if (style.fore >= 0)
				document.send(SCI_STYLESETFORE, static_cast<uptr_t>(style.id), style.fore);
			if (style.back >= 0 && style.back != styles->defaultBack)
				document.send(SCI_STYLESETBACK, static_cast<uptr_t>(style.id), style.back);
			if (style.fontStyle >= 0)
			{
				document.send(SCI_STYLESETBOLD, static_cast<uptr_t>(style.id), (style.fontStyle & 1) ? 1 : 0);
				document.send(SCI_STYLESETITALIC, static_cast<uptr_t>(style.id), (style.fontStyle & 2) ? 1 : 0);
				document.send(SCI_STYLESETUNDERLINE, static_cast<uptr_t>(style.id), (style.fontStyle & 4) ? 1 : 0);
			}
		}
	}
	else
	{
		for (const int id : language.keywordStyles)
			apply(id, theme[ThemeItem::Keyword]);
		for (const int id : language.commentStyles)
			apply(id, theme[ThemeItem::Comment]);
		for (const int id : language.stringStyles)
			apply(id, theme[ThemeItem::String]);
		for (const int id : language.numberStyles)
			apply(id, theme[ThemeItem::Number]);
		for (const int id : language.operatorStyles)
			apply(id, theme[ThemeItem::Operator]);
	}
	if (const UserLanguage *user = userLanguageFor(&language))
	{
		// Keyword groups 3-8 use other theme colours so they stay distinguishable.
		for (int group = 2; group < 8; ++group)
			apply(UserKeyword1 + group, theme[group % 2 ? ThemeItem::Preprocessor : ThemeItem::Number]);
		// Colours saved in the definition were designed for a light background.
		if (!_settings.darkTheme)
		{
			for (const auto &[id, spec] : user->styles)
			{
				if (id != UserDefault && spec.fore >= 0)
					apply(id, spec);
			}
		}
	}
}

void EditorWindow::applyViewOptions(EditorDocument &document)
{
	document.send(SCI_SETWRAPMODE, _settings.wordWrap ? SC_WRAP_WORD : SC_WRAP_NONE);
	document.send(SCI_SETVIEWWS, _settings.showWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
	document.send(SCI_SETVIEWEOL, _settings.showEndOfLine ? 1 : 0);
	document.send(SCI_SETUSETABS, _settings.useTabs ? 1 : 0);
	document.send(SCI_SETTABWIDTH, _settings.tabWidth);
	document.send(SCI_SETINDENTATIONGUIDES, _settings.indentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE);
	document.send(SCI_SETEDGEMODE, _settings.edgeColumn > 0 ? EDGE_LINE : EDGE_NONE);
	document.send(SCI_SETEDGECOLUMN, static_cast<uptr_t>(std::max(_settings.edgeColumn, 0)));
	document.send(SCI_SETWRAPVISUALFLAGS, _settings.showWrapSymbol ? SC_WRAPVISUALFLAG_END : SC_WRAPVISUALFLAG_NONE);
}

void EditorWindow::applySettings()
{
	_theme = parseTheme(_settings.darkTheme ? _settings.darkStyles : _settings.lightStyles, _settings.darkTheme);
	GtkSettings *gtkSettings = gtk_settings_get_default();
	if (gtkSettings)
		g_object_set(gtkSettings, "gtk-application-prefer-dark-theme", _settings.darkTheme ? TRUE : FALSE, nullptr);
	_updating = true;
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_wrapCheck), _settings.wordWrap);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_whitespaceCheck), _settings.showWhitespace);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_eolCheck), _settings.showEndOfLine);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_lineNumbersCheck), _settings.lineNumbers);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_darkCheck), _settings.darkTheme);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_workspaceCheck), _settings.showWorkspace);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_functionListCheck), _settings.showFunctionList);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_documentMapCheck), _settings.showDocumentMap);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_indentGuidesCheck), _settings.indentGuides);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_wrapSymbolCheck), _settings.showWrapSymbol);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_allCharactersCheck), _settings.showWhitespace && _settings.showEndOfLine);
	_updating = false;
	for (auto &document : _documents)
	{
		applyViewOptions(*document);
		applyTheme(*document);
	}
	_mapLanguage = nullptr;
	applyPanels();
	refreshDocument();
}

void EditorWindow::chooseLanguage(const Language &language)
{
	if (_updating)
		return;
	if (const auto *document = currentDocument(); document && document->language->id == language.id)
		return;
	if (auto *document = currentDocument())
	{
		document->language = &language;
		_settings.lastLanguage = language.id;
		applyTheme(*document);
		refreshDocument(document);
		scheduleFunctionList(true);
		saveSettings();
	}
}

void EditorWindow::showFind(bool replace)
{
	gtk_widget_show(_searchBar);
	gtk_widget_show(_searchEntry);
	if (auto *document = currentDocument())
	{
		const sptr_t start = std::min(document->send(SCI_GETCURRENTPOS), document->send(SCI_GETANCHOR));
		const sptr_t end = std::max(document->send(SCI_GETCURRENTPOS), document->send(SCI_GETANCHOR));
		const sptr_t length = end - start;
			if (length > 0 && length < 200)
		{
			std::string selection(static_cast<std::size_t>(length) + 1, '\0');
			document->send(SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>(selection.data()));
			selection.resize(static_cast<std::size_t>(length));
			if (selection.find_first_of("\r\n") == std::string::npos)
				gtk_entry_set_text(GTK_ENTRY(_searchEntry), selection.c_str());
		}
	}
	gtk_widget_set_visible(_replaceLabel, replace);
	gtk_widget_set_visible(_replaceEntry, replace);
	gtk_widget_set_visible(_replaceButton, replace);
	gtk_widget_set_visible(_replaceAllButton, replace);
	gtk_widget_grab_focus(replace ? _replaceEntry : _searchEntry);
	if (!replace)
		gtk_editable_select_region(GTK_EDITABLE(_searchEntry), 0, -1);
}

SearchOptions EditorWindow::searchOptions() const
{
	SearchOptions options;
	options.matchCase = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_matchCase));
	options.wholeWord = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_wholeWord));
	options.regularExpression = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_regularExpression));
	options.wrapAround = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_wrapAround));
	return options;
}

bool EditorWindow::showFindResults(const std::string &operation)
{
	EditorDocument *document = currentDocument();
	if (!document)
		return false;
	const SearchOptions options = searchOptions();
	_dialogSearchActive = false;
	const std::string needle = gtk_entry_get_text(GTK_ENTRY(_searchEntry));
	const std::string replacement = gtk_entry_get_text(GTK_ENTRY(_replaceEntry));
	if (options.regularExpression && !validateRegularExpression(needle))
	{
		gtk_label_set_text(GTK_LABEL(_messageLabel), "Expressão regular inválida.");
		return false;
	}
	std::string message;
	if (operation == "count")
		message = std::to_string(countMatches(*document, needle, options)) + " ocorrência(s).";
	else if (operation == "replaceAll")
		message = std::to_string(replaceAll(*document, needle, replacement, options)) + " ocorrência(s) substituída(s).";
	else if (operation == "replace")
	{
		const std::size_t count = replaceCurrent(*document, needle, replacement, options);
		const SearchResult result = find(*document, needle, options);
		message = count ? "Ocorrência substituída." : result.found ? "Ocorrência selecionada." : "Texto não encontrado.";
	}
	else
	{
		const SearchResult result = find(*document, needle, options, operation == "back");
		message = result.found ? (result.wrapped ? "Busca continuada na outra extremidade." : "Ocorrência encontrada.") : "Texto não encontrado.";
	}
	gtk_label_set_text(GTK_LABEL(_messageLabel), message.c_str());
	return true;
}

void EditorWindow::showError(const std::string &message)
{
	gtk_label_set_text(GTK_LABEL(_messageLabel), message.c_str());
	if (_selfTesting)
		return;
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

bool EditorWindow::saveAllModified()
{
	for (std::size_t i = 0; i < _documents.size(); ++i)
	{
		setCurrentPage(static_cast<int>(i));
		if (_documents[i]->send(SCI_GETMODIFY) && !confirmDiscard(*_documents[i]))
			return false;
	}
	return true;
}

void EditorWindow::perform(Action action)
{
	if (_updating)
		return;
	auto *document = currentDocument();
	switch (action)
	{
		case Action::NewFile:
		{
			EditorDocument *created = createDocument();
			created->eolMode = _settings.defaultEolWindows ? EolMode::Windows : EolMode::Unix;
			created->send(SCI_SETEOLMODE, _settings.defaultEolWindows ? SC_EOL_CRLF : SC_EOL_LF);
			created->encoding = _settings.newDocumentEncoding;
			if (const Language *language = languageById(_settings.defaultLanguage))
			{
				created->language = language;
				applyTheme(*created);
			}
			refreshDocument(created);
			break;
		}
		case Action::OpenFile:
		{
			GtkWidget *dialog = gtk_file_chooser_dialog_new("Abrir arquivos", GTK_WINDOW(_window), GTK_FILE_CHOOSER_ACTION_OPEN,
				"_Cancelar", GTK_RESPONSE_CANCEL, "_Abrir", GTK_RESPONSE_ACCEPT, nullptr);
			gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
			if (document && !document->path.empty())
				gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), document->path.parent_path().c_str());
			if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
			{
				GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
				for (GSList *entry = files; entry; entry = entry->next)
				{
					openPath(static_cast<const char *>(entry->data));
					g_free(entry->data);
				}
				g_slist_free(files);
			}
			gtk_widget_destroy(dialog);
			break;
		}
		case Action::SaveFile:
			if (document)
				saveDocument(*document);
			break;
		case Action::SaveFileAs:
			if (document)
				saveDocument(*document, true);
			break;
		case Action::CloseFile:
			closeDocument(currentIndex());
			break;
		case Action::Undo:
			if (document) document->send(SCI_UNDO);
			break;
		case Action::Redo:
			if (document) document->send(SCI_REDO);
			break;
		case Action::Cut:
			if (document) document->send(SCI_CUT);
			break;
		case Action::Copy:
			if (document) document->send(SCI_COPY);
			break;
		case Action::Paste:
			if (document) document->send(SCI_PASTE);
			break;
		case Action::DeleteSelection:
			if (document) document->send(SCI_CLEAR);
			break;
		case Action::SelectAll:
			if (document) document->send(SCI_SELECTALL);
			break;
		case Action::Find:
			showFindDialog(0);
			break;
		case Action::Replace:
			showFindDialog(1);
			break;
		case Action::MarkDialog:
			showFindDialog(2);
			break;
		case Action::IncrementalSearch:
			showFind(false);
			break;
		case Action::GoToLine:
		{
			if (!document || _selfTesting)
				break;
			GtkWidget *dialog = gtk_dialog_new_with_buttons("Ir para linha", GTK_WINDOW(_window), GTK_DIALOG_MODAL,
				"_Cancelar", GTK_RESPONSE_CANCEL, "_Ir", GTK_RESPONSE_ACCEPT, nullptr);
			GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
			GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
			gtk_container_set_border_width(GTK_CONTAINER(box), 12);
			gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Número da linha:"), FALSE, FALSE, 0);
			GtkWidget *spin = gtk_spin_button_new_with_range(1, std::max<sptr_t>(1, document->send(SCI_GETLINECOUNT)), 1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), document->send(SCI_LINEFROMPOSITION, document->send(SCI_GETCURRENTPOS)) + 1);
			gtk_box_pack_start(GTK_BOX(box), spin, TRUE, TRUE, 0);
			gtk_container_add(GTK_CONTAINER(content), box);
			gtk_widget_show_all(dialog);
			if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
				goToLine(*document, gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)));
			gtk_widget_destroy(dialog);
			break;
		}
		case Action::ToggleWordWrap:
			_settings.wordWrap = !_settings.wordWrap;
			applySettings();
			saveSettings();
			break;
		case Action::ToggleWhitespace:
			_settings.showWhitespace = !_settings.showWhitespace;
			applySettings();
			saveSettings();
			break;
		case Action::ToggleEndOfLine:
			_settings.showEndOfLine = !_settings.showEndOfLine;
			applySettings();
			saveSettings();
			break;
		case Action::ToggleLineNumbers:
			_settings.lineNumbers = !_settings.lineNumbers;
			applySettings();
			saveSettings();
			break;
		case Action::ToggleDarkTheme:
			_settings.darkTheme = !_settings.darkTheme;
			applySettings();
			saveSettings();
			break;
		case Action::ShowPreferences:
			showPreferences();
			break;
		case Action::ConvertToWindowsEol:
			if (document)
			{
				document->send(SCI_SETEOLMODE, SC_EOL_CRLF);
				document->send(SCI_CONVERTEOLS, SC_EOL_CRLF);
				document->eolMode = EolMode::Windows;
				refreshDocument(document);
			}
			break;
		case Action::ConvertToUnixEol:
			if (document)
			{
				document->send(SCI_SETEOLMODE, SC_EOL_LF);
				document->send(SCI_CONVERTEOLS, SC_EOL_LF);
				document->eolMode = EolMode::Unix;
				refreshDocument(document);
			}
			break;
		case Action::ZoomIn:
			if (document) document->send(SCI_ZOOMIN);
			break;
		case Action::ZoomOut:
			if (document) document->send(SCI_ZOOMOUT);
			break;
		case Action::ZoomReset:
			if (document) document->send(SCI_SETZOOM, 0);
			break;
		case Action::ShowRoadmap:
		{
			GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(_window), GTK_DIALOG_MODAL,
				GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Status do port para Linux");
			gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
				"AS 5 ETAPAS PLANEJADAS ESTÃO CONCLUÍDAS\n"
				"Edição com abas, arquivos, busca e substituição, realce de sintaxe, temas, preferências, macros, recuperação de trabalho, "
				"localizar/substituir em arquivos e pastas, lista de funções, mapa do documento, espaço de trabalho, "
				"linguagens definidas pelo usuário (formato XML do Notepad++), estilos e cores configuráveis, preferências ampliadas, "
				"plugins nativos para Linux e operações de linha.\n\n"
				"MENUS IGUAIS AO WINDOWS\n"
				"Arquivo, Editar, Pesquisar, Exibir, Codificação, Linguagem, Configurações, Ferramentas, Macro, Executar, Plugins, Janela e Ajuda, "
				"com impressão, sessões, codificações (UTF-8, UTF-16, ANSI e outras), marcadores, destaques, recolher código, editor de colunas, "
				"MD5/SHA, macros com nome e comando Executar. Ao sair, textos não salvos são guardados e reabertos sem perguntar.\n\n"
				"O QUE AINDA É DIFERENTE DO WINDOWS\n"
				"Plugins DLL do Windows não rodam no Linux: precisam ser recompilados para a interface de plugins nativa. "
				"Não há Plugin Admin, FTP, segunda vista (dividir a tela), troca de atalhos, "
				"colar HTML/RTF, painel de caracteres, histórico da área de transferência nem todas as opções das Preferências do Windows.");
			GtkWidget *dialogArea = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
			gtk_container_set_border_width(GTK_CONTAINER(dialogArea), 18);
			gtk_widget_show_all(dialog);
			gtk_dialog_run(GTK_DIALOG(dialog));
			gtk_widget_destroy(dialog);
			break;
		}
		case Action::ShowAbout:
		{
			GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(_window), GTK_DIALOG_MODAL,
				GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Notepad++ Linux 0.7.0");
			gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
				"Uma interface nativa GTK para Ubuntu e Linux Mint, criada sobre os componentes Scintilla e Lexilla do Notepad++.\n\n"
				"Não usa Wine. A versão Windows continua preservada.\n\n"
				"Plugins nativos para Linux são suportados; plugins DLL do Windows não. Veja Ajuda → Status do port.");
			gtk_dialog_run(GTK_DIALOG(dialog));
			gtk_widget_destroy(dialog);
			break;
		}
		case Action::StartMacro:
			if (_playingMacro)
				break;
			if (_recordingMacro)
				stopMacroRecording();
			_macro.clear();
			if (document)
			{
				document->send(SCI_STARTRECORD);
				_recordingMacro = true;
			}
			updateMacroState();
			break;
		case Action::StopMacro:
			stopMacroRecording();
			break;
		case Action::PlayMacro:
			playMacro();
			break;
		case Action::RunMacroAgain:
			playMacro(2);
			break;
		case Action::ClearMacro:
			if (_recordingMacro)
				stopMacroRecording();
			_macro.clear();
			_sessionStore.saveMacro(_macro);
			updateMacroState();
			break;
		case Action::BackupNow:
			backupDocuments();
			saveSession();
			gtk_label_set_text(GTK_LABEL(_messageLabel), "Backup e sessão atualizados.");
			break;
		case Action::FindInFiles:
			showFindInFiles();
			break;
		case Action::NextResult:
			nextResult(false);
			break;
		case Action::PreviousResult:
			nextResult(true);
			break;
		case Action::ToggleResults:
			if (gtk_widget_get_visible(_resultsPanel))
				hideResults();
			else
				gtk_widget_show(_resultsPanel);
			break;
		case Action::ToggleWorkspace:
			_settings.showWorkspace = !_settings.showWorkspace;
			applySettings();
			saveSettings();
			break;
		case Action::ToggleFunctionList:
			_settings.showFunctionList = !_settings.showFunctionList;
			applySettings();
			if (_settings.showFunctionList)
				gtk_notebook_set_current_page(GTK_NOTEBOOK(_sidebar), gtk_notebook_page_num(GTK_NOTEBOOK(_sidebar), _functionPanel));
			saveSettings();
			break;
		case Action::ToggleDocumentMap:
			_settings.showDocumentMap = !_settings.showDocumentMap;
			applySettings();
			if (_settings.showDocumentMap)
				gtk_notebook_set_current_page(GTK_NOTEBOOK(_sidebar), gtk_notebook_page_num(GTK_NOTEBOOK(_sidebar), _mapPanel));
			saveSettings();
			break;
		case Action::OpenWorkspaceFolder:
			chooseWorkspaceFolder();
			break;
		case Action::OpenWorkspaceFile:
		{
			if (_selfTesting)
				break;
			GtkWidget *dialog = gtk_file_chooser_dialog_new("Abrir espaço de trabalho", GTK_WINDOW(_window), GTK_FILE_CHOOSER_ACTION_OPEN,
				"_Cancelar", GTK_RESPONSE_CANCEL, "_Abrir", GTK_RESPONSE_ACCEPT, nullptr);
			GtkFileFilter *filter = gtk_file_filter_new();
			gtk_file_filter_set_name(filter, "Espaço de trabalho (*.npp-workspace)");
			gtk_file_filter_add_pattern(filter, "*.npp-workspace");
			gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
			if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
			{
				if (char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)))
				{
					openWorkspaceFile(file);
					g_free(file);
				}
			}
			gtk_widget_destroy(dialog);
			break;
		}
		case Action::SaveWorkspaceFile:
		{
			if (_selfTesting)
				break;
			if (_settings.workspaceFolders.empty())
			{
				showError("O espaço de trabalho está vazio. Adicione uma pasta antes de salvá-lo.");
				break;
			}
			GtkWidget *dialog = gtk_file_chooser_dialog_new("Salvar espaço de trabalho", GTK_WINDOW(_window), GTK_FILE_CHOOSER_ACTION_SAVE,
				"_Cancelar", GTK_RESPONSE_CANCEL, "_Salvar", GTK_RESPONSE_ACCEPT, nullptr);
			gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
			gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "projeto.npp-workspace");
			if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
			{
				if (char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)))
				{
					std::filesystem::path target = file;
					g_free(file);
					if (target.extension() != ".npp-workspace")
						target += ".npp-workspace";
					saveWorkspaceFile(target);
				}
			}
			gtk_widget_destroy(dialog);
			break;
		}
		case Action::StyleConfigurator:
			showStyleConfigurator();
			break;
		case Action::ManageUserLanguages:
			showUserLanguageDialog();
			break;
		case Action::ImportUserLanguage:
		{
			if (_selfTesting)
				break;
			GtkWidget *chooser = gtk_file_chooser_dialog_new("Importar linguagem do usuário", GTK_WINDOW(_window), GTK_FILE_CHOOSER_ACTION_OPEN,
				"_Cancelar", GTK_RESPONSE_CANCEL, "_Importar", GTK_RESPONSE_ACCEPT, nullptr);
			GtkFileFilter *filter = gtk_file_filter_new();
			gtk_file_filter_set_name(filter, "Linguagens do Notepad++ (*.xml)");
			gtk_file_filter_add_pattern(filter, "*.xml");
			gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
			if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
			{
				if (char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)))
				{
					std::string error;
					const std::size_t count = importUserLanguage(file, error);
					g_free(file);
					if (count)
						showMessage(std::to_string(count) + " linguagem(ns) importada(s). Elas aparecem em Formato → Linguagem.");
					else
						showError("Não foi possível importar.\n\n" + error);
				}
			}
			gtk_widget_destroy(chooser);
			break;
		}
		case Action::ManagePlugins:
			showPluginManager();
			break;
		case Action::OpenPluginFolder:
		{
			if (_selfTesting)
				break;
			const auto folder = PluginManager::defaultDirectories().front();
			std::error_code error;
			std::filesystem::create_directories(folder, error);
			gchar *uri = g_filename_to_uri(folder.c_str(), nullptr, nullptr);
			if (!uri || !gtk_show_uri_on_window(GTK_WINDOW(_window), uri, GDK_CURRENT_TIME, nullptr))
				showMessage("Pasta de plugins: " + folder.string());
			g_free(uri);
			break;
		}
		case Action::DuplicateLine:
			if (document) document->send(SCI_LINEDUPLICATE);
			break;
		case Action::DeleteLine:
			if (document) document->send(SCI_LINEDELETE);
			break;
		case Action::MoveLineUp:
			if (document) document->send(SCI_MOVESELECTEDLINESUP);
			break;
		case Action::MoveLineDown:
			if (document) document->send(SCI_MOVESELECTEDLINESDOWN);
			break;
		case Action::SortLinesAscending:
		case Action::SortLinesDescending:
			if (document)
				sortLines(*document, action == Action::SortLinesDescending, true);
			break;
		case Action::RemoveDuplicateLines:
			if (document)
				setStatusMessage(std::to_string(removeDuplicateLines(*document)) + " linha(s) duplicada(s) removida(s).");
			break;
		case Action::TrimTrailingWhitespace:
			if (document)
				setStatusMessage(std::to_string(trimTrailingWhitespace(*document)) + " linha(s) ajustada(s).");
			break;
		case Action::UpperCase:
			if (document) document->send(SCI_UPPERCASE);
			break;
		case Action::LowerCase:
			if (document) document->send(SCI_LOWERCASE);
			break;
		case Action::ToggleComment:
			if (document && !toggleLineComment(*document, commentPrefix(*document)))
				setStatusMessage("Esta linguagem não tem comentário de linha.");
			break;
		case Action::Quit:
			if (_recordingMacro)
				stopMacroRecording();
			// Like the Windows version's session snapshot: keep unsaved text and
			// quit without asking, as long as the backup could be written.
			if (quitKeepsUnsaved() && backupDocuments())
			{
				saveSettings();
				gtk_main_quit();
			}
			else if (saveAllModified())
			{
				backupDocuments();
				saveSession();
				gtk_main_quit();
			}
			break;
		default:
			performMore(action);
			break;
	}
	refreshDocument();
}

void EditorWindow::show()
{
	gtk_widget_show_all(_window);
	gtk_widget_hide(_searchBar);
	if (_results.empty() && _resultsStore && !gtk_tree_model_iter_n_children(GTK_TREE_MODEL(_resultsStore), nullptr))
		gtk_widget_hide(_resultsPanel);
	applyPanels();
	if (auto *document = currentDocument())
		gtk_widget_grab_focus(document->widget());
	refreshDocument();
}

void EditorWindow::addRecentFile(const std::filesystem::path &path)
{
	rememberRecentFile(_settings, path);
	rebuildRecentMenu();
	saveSettings();
}

void EditorWindow::rebuildRecentMenu()
{
	GList *recentItems = gtk_container_get_children(GTK_CONTAINER(_recentMenu));
	for (GList *entry = recentItems; entry; entry = entry->next)
		gtk_widget_destroy(GTK_WIDGET(entry->data));
	g_list_free(recentItems);
	if (_settings.recentFiles.empty())
	{
		GtkWidget *empty = gtk_menu_item_new_with_label("Nenhum arquivo recente");
		gtk_widget_set_sensitive(empty, FALSE);
		gtk_menu_shell_append(GTK_MENU_SHELL(_recentMenu), empty);
		return;
	}
	for (const auto &path : _settings.recentFiles)
	{
		GtkWidget *item = gtk_menu_item_new_with_label(path.c_str());
		attachData(item, "npp-recent-path", const_cast<std::string *>(&path));
		attachData(item, "npp-window", this);
		g_signal_connect(item, "activate", G_CALLBACK(openRecentActivated), item);
		gtk_menu_shell_append(GTK_MENU_SHELL(_recentMenu), item);
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(_recentMenu), gtk_separator_menu_item_new());
	menuItem(_recentMenu, "Abrir _todos os arquivos recentes", this, Action::OpenAllRecent, nullptr);
	menuItem(_recentMenu, "_Limpar lista de arquivos recentes", this, Action::ClearRecentList, nullptr);
	gtk_widget_show_all(_recentMenu);
}

void EditorWindow::saveSettings()
{
	if (!_selfTesting)
		_settingsStore.save(_settings);
}

std::filesystem::path sessionPath(const SettingsStore &store)
{
	return store.path().parent_path() / "session.txt";
}

bool EditorWindow::restoreEntry(const SessionEntry &entry)
{
	auto *document = createDocument();
	if (!entry.backup.empty())
		document->recoveryId = entry.backup;
	std::string error;
	const bool hasOriginal = !entry.path.empty() && std::filesystem::is_regular_file(entry.path);
	const bool hasBackup = !entry.backup.empty() && std::filesystem::is_regular_file(_sessionStore.backupPath(*document));
	if (hasOriginal)
	{
		if (!document->load(entry.path, error))
		{
			closeDocument(_documents.size() - 1, false, false);
			return false;
		}
	}
	if (hasBackup)
	{
		std::ifstream input(_sessionStore.backupPath(*document), std::ios::binary);
		std::ostringstream buffer;
		buffer << input.rdbuf();
		const std::string recovered = buffer.str();
		if (hasOriginal)
		{
			// The file on disk stays as it is; the recovered text replaces it in the
			// editor as one change, so the tab shows unsaved changes and Undo goes
			// back to what is on disk.
			if (recovered != document->text())
			{
				document->send(SCI_TARGETWHOLEDOCUMENT);
				document->send(SCI_REPLACETARGET, recovered.size(), reinterpret_cast<sptr_t>(recovered.c_str()));
				document->send(SCI_GOTOPOS, 0);
			}
		}
		else
		{
			document->setText("");
			// Unsaved text shows as unsaved, as in the Windows version.
			if (!recovered.empty())
				document->send(SCI_APPENDTEXT, recovered.size(), reinterpret_cast<sptr_t>(recovered.c_str()));
		}
		document->displayName = entry.name.empty() ? document->displayName : entry.name;
	}
	else if (!hasOriginal)
	{
		closeDocument(_documents.size() - 1, false, false);
		return false;
	}
	if (!entry.name.empty() && !hasOriginal)
		document->displayName = entry.name;
	for (const auto &known : encodings())
	{
		// Older sessions stored descriptions instead of encoding ids; those are ignored.
		if (entry.encoding == known.id)
			document->encoding = entry.encoding;
	}
	document->eolMode = static_cast<EolMode>(entry.eol);
	document->send(SCI_SETEOLMODE, entry.eol);
	if (const Language *language = languageById(entry.language))
		document->language = language;
	applyTheme(*document);
	refreshDocument(document);
	updateMacroState();
	return true;
}

void EditorWindow::loadSession()
{
	if (!_settings.restoreSession)
		return;
	for (const auto &entry : _sessionStore.load())
		restoreEntry(entry);
}

void EditorWindow::saveSession()
{
	if (!_sessionWritable || _settingsStore.path().empty() || (_selfTesting && !_selfTestDirectory.empty()))
		return;
	_sessionStore.save(_documents);
}

bool EditorWindow::backupDocuments()
{
	if (!_sessionWritable || (_selfTesting && _selfTestDirectory.empty()))
		return false;
	bool stored = true;
	for (const auto &document : _documents)
	{
		if (document->path.empty() || document->send(SCI_GETMODIFY))
		{
			// Saving a recovered document clears its old backup identity. Allocate
			// a fresh identity when later edits need a new backup, without reusing
			// or deleting the backup that belonged to the recovered session.
			if (document->recoveryId.empty())
				document->recoveryId = std::to_string(static_cast<long long>(g_get_real_time())) + "-"
					+ std::to_string(reinterpret_cast<std::uintptr_t>(document.get()));
			stored = _sessionStore.backup(*document) && stored;
		}
	}
	saveSession();
	return stored;
}

bool EditorWindow::backupTick()
{
	if (_selfTesting || _playingMacro)
		return G_SOURCE_CONTINUE;
	backupDocuments();
	return G_SOURCE_CONTINUE;
}

bool EditorWindow::recordMacro(unsigned int message, uptr_t wParam, sptr_t lParam, const char *text)
{
	if (!_recordingMacro || _playingMacro || _macro.size() >= 10000)
		return false;
	const char *commandText = text;
	if (message == SCI_REPLACESEL || message == SCI_ADDTEXT || message == SCI_INSERTTEXT || message == SCI_APPENDTEXT
		|| message == SCI_SEARCHNEXT || message == SCI_SEARCHPREV)
	{
		if (lParam != 0)
			commandText = reinterpret_cast<const char *>(lParam);
	}
	else
		commandText = nullptr;
	const MacroCommand command{message, wParam, lParam, commandText != nullptr, commandText ? commandText : ""};
	if (!SessionStore::isAllowed(command))
		return false;
	_macro.push_back(command);
	return true;
}

void EditorWindow::updateMacroState()
{
	gtk_widget_set_sensitive(_recordMenuItem, !_recordingMacro && !_playingMacro && currentDocument() != nullptr);
	gtk_widget_set_sensitive(_stopMacroMenuItem, _recordingMacro);
	gtk_widget_set_sensitive(_playMacroMenuItem, !_recordingMacro && !_playingMacro && !_macro.empty() && currentDocument() != nullptr);
	gtk_widget_set_sensitive(_repeatMacroMenuItem, !_recordingMacro && !_playingMacro && currentDocument() != nullptr);
	gtk_widget_set_sensitive(_clearMacroMenuItem, !_recordingMacro && !_playingMacro && !_macro.empty());
	if (_recordingMacro)
		gtk_label_set_text(GTK_LABEL(_messageLabel), "Gravando macro…");
	else if (!_playingMacro)
		gtk_label_set_text(GTK_LABEL(_messageLabel), _macro.empty() ? "Nenhuma macro gravada." : "Macro pronta para reprodução.");
}

void EditorWindow::moveRecordingToCurrentPage()
{
	if (!_recordingMacro || _playingMacro)
		return;
	for (auto &document : _documents)
		document->send(SCI_STOPRECORD);
	if (auto *document = currentDocument())
		document->send(SCI_STARTRECORD);
}

void EditorWindow::stopMacroRecording()
{
	if (!_recordingMacro)
		return;
	for (auto &document : _documents)
		document->send(SCI_STOPRECORD);
	_recordingMacro = false;
	_sessionStore.saveMacro(_macro);
	updateMacroState();
}

bool EditorWindow::playMacro(std::size_t repetitions)
{
	if (_recordingMacro || _playingMacro || _macro.empty())
		return false;
	if (std::any_of(_macro.begin(), _macro.end(), [](const MacroCommand &command) { return !SessionStore::isAllowed(command); }))
		return false;
	auto *document = currentDocument();
	if (!document)
		return false;
	const auto function = reinterpret_cast<SciFnDirect>(document->send(SCI_GETDIRECTFUNCTION));
	const sptr_t pointer = document->send(SCI_GETDIRECTPOINTER);
	if (!function || !pointer)
		return false;
	_playingMacro = true;
	document->send(SCI_BEGINUNDOACTION);
	for (std::size_t repetition = 0; repetition < std::min<std::size_t>(repetitions, 100); ++repetition)
	{
		for (const auto &command : _macro)
			function(pointer, command.message, command.wParam, command.hasText ? reinterpret_cast<sptr_t>(command.text.c_str()) : command.lParam);
	}
	document->send(SCI_ENDUNDOACTION);
	document->send(SCI_SCROLLCARET);
	_playingMacro = false;
	updateMacroState();
	refreshDocument(document);
	backupDocuments();
	saveSession();
	return true;
}

void EditorWindow::buildResultsPanel()
{
	_resultsPanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(_resultsPanel), "npp-results");
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(header), 4);
	_resultsHeader = gtk_label_new("Resultados da busca");
	gtk_label_set_xalign(GTK_LABEL(_resultsHeader), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(_resultsHeader), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(header), _resultsHeader, TRUE, TRUE, 0);
	GtkWidget *close = gtk_button_new_with_label("×");
	gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(close, "Fechar painel de resultados (F7)");
	g_signal_connect(close, "clicked", G_CALLBACK(closeResultsClicked), this);
	gtk_box_pack_end(GTK_BOX(header), close, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(_resultsPanel), header, FALSE, FALSE, 0);

	_resultsStore = gtk_tree_store_new(ResultColumns, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);
	_resultsView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(_resultsStore));
	g_object_unref(_resultsStore);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(_resultsView), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(_resultsView), FALSE);
	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	g_object_set(renderer, "family", "Monospace", "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
	GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Resultado", renderer,
		"text", ResultText, "weight", ResultWeight, nullptr);
	gtk_tree_view_column_set_expand(column, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(_resultsView), column);
	gtk_widget_set_tooltip_text(_resultsView, "Clique duas vezes ou pressione Enter para abrir o resultado. F4 e Shift+F4 percorrem os resultados.");
	g_signal_connect(_resultsView, "row-activated", G_CALLBACK(resultRowActivated), this);
	GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroller, -1, 120);
	gtk_container_add(GTK_CONTAINER(scroller), _resultsView);
	gtk_box_pack_start(GTK_BOX(_resultsPanel), scroller, TRUE, TRUE, 0);
}

void EditorWindow::showFindInFiles()
{
	if (!_findDialog)
	{
		_findDialog = gtk_dialog_new_with_buttons("Localizar em arquivos", GTK_WINDOW(_window), GTK_DIALOG_DESTROY_WITH_PARENT,
			"Substituir nos a_bertos", ReplaceAllInOpenDocuments,
			"Localizar nos _abertos", FindAllInOpenDocuments,
			"_Substituir em arquivos", ReplaceAllInFiles,
			"Localizar _todos", FindAllInFiles,
			"_Fechar", GTK_RESPONSE_CLOSE, nullptr);
		gtk_dialog_set_default_response(GTK_DIALOG(_findDialog), FindAllInFiles);
		g_signal_connect(_findDialog, "response", G_CALLBACK(findDialogResponse), this);
		g_signal_connect(_findDialog, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
		GtkWidget *grid = gtk_grid_new();
		gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
		gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
		gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
		auto addRow = [&](int row, const char *caption, GtkWidget *widget, GtkWidget *extra = nullptr) {
			GtkWidget *label = gtk_label_new_with_mnemonic(caption);
			gtk_label_set_xalign(GTK_LABEL(label), 1.0);
			gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);
			gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
			gtk_widget_set_hexpand(widget, TRUE);
			gtk_grid_attach(GTK_GRID(grid), widget, 1, row, extra ? 1 : 2, 1);
			if (extra)
				gtk_grid_attach(GTK_GRID(grid), extra, 2, row, 1, 1);
		};
		_findText = gtk_entry_new();
		gtk_entry_set_activates_default(GTK_ENTRY(_findText), TRUE);
		gtk_entry_set_width_chars(GTK_ENTRY(_findText), 40);
		addRow(0, "_Localizar:", _findText);
		_findReplacement = gtk_entry_new();
		gtk_entry_set_placeholder_text(GTK_ENTRY(_findReplacement), "Usado apenas pelos botões Substituir");
		addRow(1, "Substituir _por:", _findReplacement);
		_findFilters = gtk_entry_new();
		gtk_widget_set_tooltip_text(_findFilters,
			"Padrões separados por espaço ou ponto e vírgula, por exemplo: *.cpp *.h\n"
			"Use ! para excluir arquivos (!*.min.js) ou pastas (!build/ ou !+\\node_modules).");
		addRow(2, "_Filtros:", _findFilters);
		_findFolder = gtk_entry_new();
		GtkWidget *choose = gtk_button_new_with_mnemonic("_Escolher…");
		g_signal_connect(choose, "clicked", G_CALLBACK(chooseFolderClicked), this);
		addRow(3, "Pa_sta:", _findFolder, choose);
		GtkWidget *scope = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		_findRecursive = gtk_check_button_new_with_mnemonic("Incluir s_ubpastas");
		_findHidden = gtk_check_button_new_with_mnemonic("Incluir arquivos e pastas _ocultos");
		gtk_box_pack_start(GTK_BOX(scope), _findRecursive, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(scope), _findHidden, FALSE, FALSE, 0);
		gtk_grid_attach(GTK_GRID(grid), scope, 1, 4, 2, 1);
		GtkWidget *match = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		_findMatchCase = gtk_check_button_new_with_mnemonic("Diferenciar _maiúsculas");
		_findWholeWord = gtk_check_button_new_with_mnemonic("Palavra _inteira");
		_findRegex = gtk_check_button_new_with_mnemonic("E_xpressão regular");
		gtk_widget_set_tooltip_text(_findRegex, "Expressões regulares do Scintilla: use \\(grupo\\) e \\1 na substituição");
		gtk_box_pack_start(GTK_BOX(match), _findMatchCase, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(match), _findWholeWord, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(match), _findRegex, FALSE, FALSE, 0);
		gtk_grid_attach(GTK_GRID(grid), match, 1, 5, 2, 1);
		_findDialogStatus = gtk_label_new("Arquivos binários, maiores que 64 MB e ocultos (salvo opção acima) são ignorados.");
		gtk_label_set_xalign(GTK_LABEL(_findDialogStatus), 0.0);
		gtk_label_set_line_wrap(GTK_LABEL(_findDialogStatus), TRUE);
		gtk_grid_attach(GTK_GRID(grid), _findDialogStatus, 0, 6, 3, 1);
		gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(_findDialog))), grid);
		gtk_entry_set_text(GTK_ENTRY(_findFilters), _settings.findFilters.c_str());
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_findRecursive), _settings.findRecursive);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_findHidden), _settings.findHidden);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_findMatchCase), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_matchCase)));
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_findWholeWord), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_wholeWord)));
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_findRegex), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_regularExpression)));
	}
	const EditorDocument *document = currentDocument();
	std::string text = selectedLine(document);
	if (text.empty())
		text = gtk_entry_get_text(GTK_ENTRY(_findText));
	if (text.empty())
		text = gtk_entry_get_text(GTK_ENTRY(_searchEntry));
	gtk_entry_set_text(GTK_ENTRY(_findText), text.c_str());
	if (std::string(gtk_entry_get_text(GTK_ENTRY(_findFolder))).empty())
	{
		std::error_code error;
		std::filesystem::path folder = _settings.findFolder;
		if (folder.empty() || !std::filesystem::is_directory(folder, error))
			folder = document && !document->path.empty() ? document->path.parent_path() : std::filesystem::path(g_get_home_dir());
		gtk_entry_set_text(GTK_ENTRY(_findFolder), folder.c_str());
	}
	gtk_widget_show_all(_findDialog);
	gtk_window_present(GTK_WINDOW(_findDialog));
	gtk_widget_grab_focus(_findText);
}

void EditorWindow::chooseFindFolder()
{
	GtkWidget *chooser = gtk_file_chooser_dialog_new("Escolher pasta", GTK_WINDOW(_findDialog ? _findDialog : _window),
		GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancelar", GTK_RESPONSE_CANCEL, "_Selecionar", GTK_RESPONSE_ACCEPT, nullptr);
	const std::string current = _findFolder ? gtk_entry_get_text(GTK_ENTRY(_findFolder)) : "";
	if (!current.empty())
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), current.c_str());
	if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
	{
		if (char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)))
		{
			gtk_entry_set_text(GTK_ENTRY(_findFolder), folder);
			g_free(folder);
		}
	}
	gtk_widget_destroy(chooser);
}

void EditorWindow::runFindInFilesDialog(int response)
{
	if (!_findDialog)
		return;
	if (response != FindAllInFiles && response != ReplaceAllInFiles
		&& response != FindAllInOpenDocuments && response != ReplaceAllInOpenDocuments)
	{
		gtk_widget_hide(_findDialog);
		return;
	}
	const std::string needle = gtk_entry_get_text(GTK_ENTRY(_findText));
	const std::string replacement = gtk_entry_get_text(GTK_ENTRY(_findReplacement));
	FileSearchOptions options;
	options.filters = gtk_entry_get_text(GTK_ENTRY(_findFilters));
	options.recursive = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_findRecursive));
	options.includeHidden = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_findHidden));
	options.text.matchCase = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_findMatchCase));
	options.text.wholeWord = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_findWholeWord));
	options.text.regularExpression = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_findRegex));
	const std::filesystem::path folder = gtk_entry_get_text(GTK_ENTRY(_findFolder));
	if (needle.empty())
	{
		gtk_label_set_text(GTK_LABEL(_findDialogStatus), "Digite o texto a localizar.");
		return;
	}
	if (options.text.regularExpression && !validateRegularExpression(needle))
	{
		gtk_label_set_text(GTK_LABEL(_findDialogStatus), "Expressão regular inválida.");
		return;
	}
	if (response == FindAllInFiles || response == ReplaceAllInFiles)
	{
		_settings.findFolder = folder.string();
		_settings.findFilters = options.filters;
		_settings.findRecursive = options.recursive;
		_settings.findHidden = options.includeHidden;
		saveSettings();
	}
	gtk_label_set_text(GTK_LABEL(_findDialogStatus), "Pesquisando…");
	GdkWindow *surface = gtk_widget_get_window(_findDialog);
	GdkCursor *busy = surface ? gdk_cursor_new_from_name(gdk_window_get_display(surface), "wait") : nullptr;
	if (surface)
		gdk_window_set_cursor(surface, busy);
	switch (response)
	{
		case FindAllInFiles:
			findInFiles(folder, needle, options);
			break;
		case ReplaceAllInFiles:
			replaceInFiles(folder, needle, replacement, options, true);
			break;
		case FindAllInOpenDocuments:
			findInOpenDocuments(needle, options.text);
			break;
		default:
			replaceInOpenDocuments(needle, replacement, options.text);
	}
	if (surface)
		gdk_window_set_cursor(surface, nullptr);
	if (busy)
		g_object_unref(busy);
	gtk_label_set_text(GTK_LABEL(_findDialogStatus), gtk_label_get_text(GTK_LABEL(_resultsHeader)));
}

OpenDocumentLookup EditorWindow::openDocumentLookup() const
{
	struct Identity
	{
		dev_t device;
		ino_t inode;
		EditorDocument *document;
	};
	std::vector<Identity> identities;
	for (const auto &document : _documents)
	{
		struct stat information {};
		if (!document->path.empty() && ::stat(document->path.c_str(), &information) == 0)
			identities.push_back({information.st_dev, information.st_ino, document.get()});
	}
	return [identities](const std::filesystem::path &path) -> EditorDocument * {
		if (identities.empty())
			return nullptr;
		struct stat information {};
		if (::stat(path.c_str(), &information) != 0)
			return nullptr;
		for (const auto &identity : identities)
		{
			if (identity.device == information.st_dev && identity.inode == information.st_ino)
				return identity.document;
		}
		return nullptr;
	};
}

FileSearchSummary EditorWindow::findInFiles(const std::filesystem::path &folder, const std::string &needle,
	const FileSearchOptions &options)
{
	if (!_fileSearcher)
		_fileSearcher = std::make_unique<FileSearcher>();
	FileSearchSummary summary = _fileSearcher->find(folder, needle, options, openDocumentLookup());
	showResults("Localizar “" + needle + "” em " + folder.string(), summary, false);
	return summary;
}

bool EditorWindow::confirmFileReplacement(const std::filesystem::path &folder, const FileSearchOptions &options,
	const std::string &needle, const std::string &replacement)
{
	if (_selfTesting)
		return true;
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(_findDialog ? _findDialog : _window), GTK_DIALOG_MODAL,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "Substituir “%s” por “%s” em todos os arquivos?", needle.c_str(), replacement.c_str());
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
		"Pasta: %s\nFiltros: %s\nSubpastas: %s\n\n"
		"Arquivos fechados serão gravados diretamente no disco e essa alteração não pode ser desfeita pelo editor. "
		"Arquivos já abertos são alterados somente no editor; revise e salve-os depois.",
		folder.c_str(), options.filters.c_str(), options.recursive ? "sim" : "não");
	gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Cancelar", GTK_RESPONSE_CANCEL, "_Substituir", GTK_RESPONSE_ACCEPT, nullptr);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
	const bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
	gtk_widget_destroy(dialog);
	return accepted;
}

FileSearchSummary EditorWindow::replaceInFiles(const std::filesystem::path &folder, const std::string &needle,
	const std::string &replacement, const FileSearchOptions &options, bool confirm)
{
	if (confirm && !confirmFileReplacement(folder, options, needle, replacement))
	{
		FileSearchSummary cancelled;
		cancelled.error = "Substituição cancelada.";
		return cancelled;
	}
	if (!_fileSearcher)
		_fileSearcher = std::make_unique<FileSearcher>();
	FileSearchSummary summary = _fileSearcher->replace(folder, needle, replacement, options, openDocumentLookup());
	for (const auto &file : summary.files)
	{
		if (file.document)
			refreshDocument(const_cast<EditorDocument *>(file.document));
	}
	showResults("Substituir “" + needle + "” por “" + replacement + "” em " + folder.string(), summary, true);
	return summary;
}

FileSearchSummary EditorWindow::findInOpenDocuments(const std::string &needle, const SearchOptions &options, bool onlyCurrent)
{
	FileSearchSummary summary;
	const std::size_t limit = FileSearchOptions().maxMatches;
	std::size_t total = 0;
	const EditorDocument *current = currentDocument();
	for (const auto &document : _documents)
	{
		if (onlyCurrent && document.get() != current)
			continue;
		FileResult result;
		result.path = document->path;
		result.name = document->displayName;
		result.document = document.get();
		if (!FileSearcher::collectMatches(*document, needle, options, result, limit - total))
			summary.truncated = true;
		++summary.filesScanned;
		total += result.matches.size();
		if (!result.matches.empty())
			summary.files.push_back(std::move(result));
		if (total >= limit)
			break;
	}
	showResults("Localizar “" + needle + (onlyCurrent ? "” no documento atual" : "” nos documentos abertos"), summary, false);
	return summary;
}

FileSearchSummary EditorWindow::replaceInOpenDocuments(const std::string &needle, const std::string &replacement,
	const SearchOptions &options)
{
	FileSearchSummary summary;
	for (const auto &document : _documents)
	{
		FileResult result;
		result.path = document->path;
		result.name = document->displayName;
		result.document = document.get();
		result.replaced = replaceAll(*document, needle, replacement, options);
		++summary.filesScanned;
		if (result.replaced)
		{
			refreshDocument(document.get());
			summary.files.push_back(std::move(result));
		}
	}
	showResults("Substituir “" + needle + "” por “" + replacement + "” nos documentos abertos", summary, true);
	return summary;
}

void EditorWindow::showResults(const std::string &title, const FileSearchSummary &summary, bool replacing)
{
	gtk_tree_store_clear(_resultsStore);
	_results.clear();
	_currentResult = static_cast<std::size_t>(-1);
	for (const auto &file : summary.files)
	{
		const std::string name = file.path.empty() ? file.name : file.path.string();
		std::string label = name + " — ";
		if (!file.error.empty())
			label += "não alterado: " + file.error;
		else if (replacing)
			label += plural(file.replaced, "substituição", "substituições") + (file.document ? " no editor (ainda não salvo)" : " gravada(s) no disco");
		else
			label += plural(file.matches.size(), "ocorrência", "ocorrências") + (file.document ? " (conteúdo aberto no editor)" : "");
		GtkTreeIter parent;
		gtk_tree_store_append(_resultsStore, &parent, nullptr);
		int headerIndex = -1;
		if (replacing)
		{
			ResultLocation location;
			location.path = file.path;
			location.document = file.document;
			location.line = 1;
			headerIndex = static_cast<int>(_results.size());
			_results.push_back(location);
		}
		else if (!file.matches.empty())
			headerIndex = static_cast<int>(_results.size());
		gtk_tree_store_set(_resultsStore, &parent, ResultText, label.c_str(), ResultIndex, headerIndex, ResultWeight, PANGO_WEIGHT_BOLD, -1);
		if (replacing)
		{
			gchar *rowPath = gtk_tree_model_get_string_from_iter(GTK_TREE_MODEL(_resultsStore), &parent);
			_results.back().rowPath = rowPath;
			g_free(rowPath);
		}
		for (const auto &match : file.matches)
		{
			const std::string text = "Linha " + std::to_string(match.line) + ", Col " + std::to_string(match.column) + ":  " + match.lineText;
			GtkTreeIter child;
			gtk_tree_store_append(_resultsStore, &child, &parent);
			gtk_tree_store_set(_resultsStore, &child, ResultText, text.c_str(), ResultIndex, static_cast<int>(_results.size()), ResultWeight, PANGO_WEIGHT_NORMAL, -1);
			ResultLocation location;
			location.path = file.path;
			location.document = file.document;
			location.start = match.start;
			location.end = match.end;
			location.line = match.line;
			gchar *rowPath = gtk_tree_model_get_string_from_iter(GTK_TREE_MODEL(_resultsStore), &child);
			location.rowPath = rowPath;
			g_free(rowPath);
			_results.push_back(std::move(location));
		}
	}
	std::string header = title + ": ";
	if (!summary.error.empty())
		header += summary.error;
	else if (replacing)
		header += plural(summary.totalReplaced(), "substituição", "substituições") + " em "
			+ plural(summary.filesWithMatches(), "arquivo", "arquivos");
	else
		header += plural(summary.totalMatches(), "ocorrência", "ocorrências") + " em "
			+ plural(summary.filesWithMatches(), "arquivo", "arquivos");
	if (summary.error.empty())
		header += " (" + plural(summary.filesScanned, "arquivo pesquisado", "arquivos pesquisados") + ")";
	if (summary.filesWithErrors())
		header += "; " + plural(summary.filesWithErrors(), "arquivo não alterado", "arquivos não alterados");
	if (summary.skippedBinary)
		header += "; " + plural(summary.skippedBinary, "binário ignorado", "binários ignorados");
	if (summary.skippedLarge)
		header += "; " + plural(summary.skippedLarge, "arquivo grande ignorado", "arquivos grandes ignorados");
	if (summary.unreadable)
		header += "; " + plural(summary.unreadable, "item ilegível", "itens ilegíveis");
	if (summary.truncated)
		header += "; limite atingido, refine a busca";
	header += ".";
	gtk_label_set_text(GTK_LABEL(_resultsHeader), header.c_str());
	gtk_label_set_text(GTK_LABEL(_messageLabel), header.c_str());
	gtk_tree_view_expand_all(GTK_TREE_VIEW(_resultsView));
	gtk_widget_show_all(_resultsPanel);
	if (!_resultsPositioned)
	{
		const int height = gtk_widget_get_allocated_height(_paned);
		if (height > 360)
		{
			gtk_paned_set_position(GTK_PANED(_paned), height - 220);
			_resultsPositioned = true;
		}
	}
}

void EditorWindow::hideResults()
{
	gtk_widget_hide(_resultsPanel);
	if (auto *document = currentDocument())
		gtk_widget_grab_focus(document->widget());
}

bool EditorWindow::goToResult(std::size_t index)
{
	if (index >= _results.size())
		return false;
	const ResultLocation &location = _results[index];
	EditorDocument *document = nullptr;
	if (location.document)
	{
		for (std::size_t i = 0; i < _documents.size(); ++i)
		{
			if (_documents[i].get() == location.document && _documents[i]->path == location.path)
			{
				setCurrentPage(static_cast<int>(i));
				document = _documents[i].get();
				break;
			}
		}
	}
	if (!document && !location.path.empty())
	{
		openPath(location.path);
		EditorDocument *current = currentDocument();
		if (current && current->path == std::filesystem::absolute(location.path).lexically_normal())
			document = current;
	}
	if (!document)
	{
		gtk_label_set_text(GTK_LABEL(_messageLabel), "O documento deste resultado não está mais aberto.");
		return false;
	}
	const sptr_t length = document->send(SCI_GETLENGTH);
	const sptr_t line = static_cast<sptr_t>(location.line > 0 ? location.line - 1 : 0);
	document->send(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(line));
	if (location.end <= length && location.start <= location.end)
		document->send(SCI_SETSEL, location.start, location.end);
	else
		goToLine(*document, static_cast<long>(line) + 1);
	document->send(SCI_SCROLLCARET);
	_currentResult = index;
	if (!location.rowPath.empty())
	{
		GtkTreePath *path = gtk_tree_path_new_from_string(location.rowPath.c_str());
		gtk_tree_view_expand_to_path(GTK_TREE_VIEW(_resultsView), path);
		gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(_resultsView)), path);
		if (gtk_widget_get_realized(_resultsView))
			gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(_resultsView), path, nullptr, FALSE, 0, 0);
		gtk_tree_path_free(path);
	}
	if (!_selfTesting)
		gtk_widget_grab_focus(document->widget());
	const std::string message = "Resultado " + std::to_string(index + 1) + " de " + std::to_string(_results.size()) + ".";
	gtk_label_set_text(GTK_LABEL(_messageLabel), message.c_str());
	return true;
}

bool EditorWindow::nextResult(bool backwards)
{
	if (_results.empty())
	{
		gtk_label_set_text(GTK_LABEL(_messageLabel), "Não há resultados de busca. Use Pesquisar → Localizar em arquivos.");
		return false;
	}
	const std::size_t count = _results.size();
	std::size_t index;
	if (_currentResult >= count)
		index = backwards ? count - 1 : 0;
	else
		index = backwards ? (_currentResult + count - 1) % count : (_currentResult + 1) % count;
	return goToResult(index);
}

void EditorWindow::activateResultRow(GtkTreePath *path)
{
	GtkTreeIter iter;
	if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(_resultsStore), &iter, path))
		return;
	int index = -1;
	gtk_tree_model_get(GTK_TREE_MODEL(_resultsStore), &iter, ResultIndex, &index, -1);
	if (index >= 0)
		goToResult(static_cast<std::size_t>(index));
}

void EditorWindow::buildWorkspacePanel()
{
	_workspacePanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(_workspacePanel), "npp-workspace");
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_container_set_border_width(GTK_CONTAINER(header), 3);
	GtkWidget *title = gtk_label_new("Espaço de trabalho");
	gtk_label_set_xalign(GTK_LABEL(title), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 4);
	GtkWidget *add = iconButton("list-add", "Adicionar pasta ao espaço de trabalho");
	GtkWidget *remove = iconButton("list-remove", "Remover do espaço de trabalho a pasta selecionada (não apaga arquivos)");
	GtkWidget *refresh = iconButton("view-refresh", "Atualizar a árvore de pastas");
	GtkWidget *find = iconButton("edit-find", "Localizar em arquivos na pasta selecionada");
	g_signal_connect(add, "clicked", G_CALLBACK(workspaceAddClicked), this);
	g_signal_connect(remove, "clicked", G_CALLBACK(workspaceRemoveClicked), this);
	g_signal_connect(refresh, "clicked", G_CALLBACK(workspaceRefreshClicked), this);
	g_signal_connect(find, "clicked", G_CALLBACK(workspaceFindClicked), this);
	for (GtkWidget *button : {add, remove, refresh, find})
		gtk_box_pack_start(GTK_BOX(header), button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(_workspacePanel), header, FALSE, FALSE, 0);

	_workspaceStore = gtk_tree_store_new(WorkspaceColumns, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING);
	_workspaceView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(_workspaceStore));
	g_object_unref(_workspaceStore);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(_workspaceView), FALSE);
	gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(_workspaceView), WorkspacePath);
	gtk_tree_view_set_search_column(GTK_TREE_VIEW(_workspaceView), WorkspaceName);
	GtkTreeViewColumn *column = gtk_tree_view_column_new();
	GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
	gtk_tree_view_column_pack_start(column, icon, FALSE);
	gtk_tree_view_column_add_attribute(column, icon, "icon-name", WorkspaceIcon);
	GtkCellRenderer *text = gtk_cell_renderer_text_new();
	g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, nullptr);
	gtk_tree_view_column_pack_start(column, text, TRUE);
	gtk_tree_view_column_add_attribute(column, text, "text", WorkspaceName);
	gtk_tree_view_append_column(GTK_TREE_VIEW(_workspaceView), column);
	g_signal_connect(_workspaceView, "row-activated", G_CALLBACK(workspaceRowActivated), this);
	g_signal_connect(_workspaceView, "test-expand-row", G_CALLBACK(workspaceTestExpand), this);
	GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroller), _workspaceView);
	gtk_box_pack_start(GTK_BOX(_workspacePanel), scroller, TRUE, TRUE, 0);
	gtk_widget_set_size_request(_workspacePanel, 220, -1);
	rebuildWorkspace();
}

void EditorWindow::buildSidebar()
{
	_sidebar = gtk_notebook_new();
	gtk_notebook_set_show_border(GTK_NOTEBOOK(_sidebar), FALSE);
	gtk_widget_set_size_request(_sidebar, 250, -1);

	_functionPanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_container_set_border_width(GTK_CONTAINER(_functionPanel), 4);
	GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	_functionFilter = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(_functionFilter), "Filtrar funções");
	g_signal_connect(_functionFilter, "changed", G_CALLBACK(functionFilterChanged), this);
	gtk_box_pack_start(GTK_BOX(tools), _functionFilter, TRUE, TRUE, 0);
	_functionSort = gtk_toggle_button_new_with_label("A–Z");
	gtk_widget_set_tooltip_text(_functionSort, "Ordenar por nome em vez da ordem do arquivo");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_functionSort), _settings.functionListSorted);
	g_signal_connect(_functionSort, "toggled", G_CALLBACK(functionSortToggled), this);
	gtk_box_pack_start(GTK_BOX(tools), _functionSort, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(_functionPanel), tools, FALSE, FALSE, 0);
	_functionStore = gtk_tree_store_new(FunctionColumns, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);
	_functionView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(_functionStore));
	g_object_unref(_functionStore);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(_functionView), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(_functionView), FALSE);
	GtkCellRenderer *name = gtk_cell_renderer_text_new();
	g_object_set(name, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
	GtkTreeViewColumn *nameColumn = gtk_tree_view_column_new_with_attributes("Nome", name, "text", FunctionText, "weight", FunctionWeight, nullptr);
	gtk_tree_view_column_set_expand(nameColumn, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(_functionView), nameColumn);
	GtkCellRenderer *kind = gtk_cell_renderer_text_new();
	g_object_set(kind, "foreground", "gray", "scale", 0.85, nullptr);
	gtk_tree_view_append_column(GTK_TREE_VIEW(_functionView), gtk_tree_view_column_new_with_attributes("Tipo", kind, "text", FunctionKind, nullptr));
	g_signal_connect(_functionView, "row-activated", G_CALLBACK(functionRowActivated), this);
	gtk_widget_set_tooltip_text(_functionView, "Clique duas vezes ou pressione Enter para ir até a definição.");
	GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroller), _functionView);
	gtk_box_pack_start(GTK_BOX(_functionPanel), scroller, TRUE, TRUE, 0);
	_functionStatus = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(_functionStatus), 0.0);
	gtk_label_set_line_wrap(GTK_LABEL(_functionStatus), TRUE);
	gtk_box_pack_start(GTK_BOX(_functionPanel), _functionStatus, FALSE, FALSE, 0);
	gtk_notebook_append_page(GTK_NOTEBOOK(_sidebar), _functionPanel, gtk_label_new("Funções"));

	_mapPanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *mapWidget = scintilla_new();
	_mapWidget = mapWidget;
	_mapView = std::make_unique<EditorDocument>(mapWidget);
	g_signal_connect(mapWidget, SCINTILLA_NOTIFY, G_CALLBACK(mapNotified), this);
	gtk_widget_set_can_focus(mapWidget, FALSE);
	gtk_widget_set_tooltip_text(mapWidget, "Mapa do documento: clique ou arraste para navegar");
	gtk_widget_add_events(mapWidget, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_MOTION_MASK);
	g_signal_connect(mapWidget, "button-press-event", G_CALLBACK(mapButtonPressed), this);
	g_signal_connect(mapWidget, "button-release-event", G_CALLBACK(swallowEvent), nullptr);
	g_signal_connect(mapWidget, "motion-notify-event", G_CALLBACK(mapMotion), this);
	g_signal_connect(mapWidget, "scroll-event", G_CALLBACK(mapScrolled), this);
	g_signal_connect(mapWidget, "key-press-event", G_CALLBACK(swallowEvent), nullptr);
	gtk_box_pack_start(GTK_BOX(_mapPanel), mapWidget, TRUE, TRUE, 0);
	gtk_notebook_append_page(GTK_NOTEBOOK(_sidebar), _mapPanel, gtk_label_new("Mapa"));
}

void EditorWindow::applyPanels()
{
	if (!_sidebar || !_workspacePanel)
		return;
	gtk_widget_set_visible(_workspacePanel, _settings.showWorkspace);
	gtk_widget_set_visible(_functionPanel, _settings.showFunctionList);
	gtk_widget_set_visible(_mapPanel, _settings.showDocumentMap);
	gtk_widget_set_visible(_sidebar, _settings.showFunctionList || _settings.showDocumentMap);
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(_sidebar), _settings.showFunctionList && _settings.showDocumentMap);
	if (_settings.showFunctionList)
		scheduleFunctionList(true);
	syncDocumentMap();
}

void EditorWindow::scheduleFunctionList(bool immediate)
{
	if (!_settings.showFunctionList || !_functionStore)
		return;
	if (_functionTimer)
	{
		g_source_remove(_functionTimer);
		_functionTimer = 0;
	}
	if (immediate)
		refreshFunctionList();
	else if (!_selfTesting)
		_functionTimer = g_timeout_add(700, functionTimerFired, this);
}

void EditorWindow::refreshFunctionList()
{
	if (!_functionStore || !_settings.showFunctionList)
		return;
	_settings.functionListSorted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(_functionSort));
	gtk_tree_store_clear(_functionStore);
	_functions.clear();
	EditorDocument *document = currentDocument();
	_functionDocument = document;
	if (!document)
	{
		gtk_label_set_text(GTK_LABEL(_functionStatus), "Nenhum documento aberto.");
		return;
	}
	if (!functionListSupports(document->language->id))
	{
		const std::string message = "A lista de funções não está disponível para " + document->language->name + ".";
		gtk_label_set_text(GTK_LABEL(_functionStatus), message.c_str());
		return;
	}
	_functions = parseFunctions(document->language->id, document->text());
	const std::string filter = folded(gtk_entry_get_text(GTK_ENTRY(_functionFilter)));
	const bool sorted = _settings.functionListSorted;
	auto sortByName = [&](std::vector<std::size_t> &indices) {
		if (sorted)
			std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
				return folded(_functions[a].name) < folded(_functions[b].name);
			});
	};
	auto append = [&](GtkTreeIter *parent, std::size_t index, const std::string &label) {
		GtkTreeIter iter;
		gtk_tree_store_append(_functionStore, &iter, parent);
		const auto &entry = _functions[index];
		gtk_tree_store_set(_functionStore, &iter, FunctionText, label.c_str(), FunctionKind, entry.kind.c_str(),
			FunctionIndex, static_cast<int>(index), FunctionWeight, entry.container ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, -1);
		return iter;
	};
	std::size_t shown = 0;
	if (!filter.empty())
	{
		std::vector<std::size_t> matches;
		for (std::size_t i = 0; i < _functions.size(); ++i)
		{
			if (folded(_functions[i].name).find(filter) != std::string::npos)
				matches.push_back(i);
		}
		sortByName(matches);
		for (const std::size_t index : matches)
		{
			const auto &entry = _functions[index];
			append(nullptr, index, entry.parent.empty() ? entry.name : entry.parent + " › " + entry.name);
		}
		shown = matches.size();
	}
	else
	{
		// Build the tree in file order: a container owns the entries whose parent names it.
		std::vector<std::vector<std::size_t>> children(_functions.size() + 1);
		std::map<std::string, std::size_t> containers;
		for (std::size_t i = 0; i < _functions.size(); ++i)
		{
			const auto &entry = _functions[i];
			const auto parent = entry.parent.empty() ? containers.end() : containers.find(entry.parent);
			children[parent == containers.end() ? _functions.size() : parent->second].push_back(i);
			if (entry.container)
				containers[entry.name] = i;
		}
		std::function<void(GtkTreeIter *, std::size_t)> insert = [&](GtkTreeIter *parent, std::size_t node) {
			std::vector<std::size_t> list = children[node];
			sortByName(list);
			for (const std::size_t index : list)
			{
				const auto &entry = _functions[index];
				const bool nested = parent != nullptr;
				GtkTreeIter iter = append(parent, index, nested || entry.parent.empty() ? entry.name : entry.parent + " › " + entry.name);
				insert(&iter, index);
			}
		};
		insert(nullptr, _functions.size());
		shown = _functions.size();
		gtk_tree_view_expand_all(GTK_TREE_VIEW(_functionView));
	}
	std::string status = shown == 1 ? "1 item" : std::to_string(shown) + " itens";
	if (!filter.empty())
		status += " de " + std::to_string(_functions.size());
	status += " · " + document->language->name;
	gtk_label_set_text(GTK_LABEL(_functionStatus), status.c_str());
}

void EditorWindow::activateFunctionRow(GtkTreePath *path)
{
	GtkTreeIter iter;
	if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(_functionStore), &iter, path))
		return;
	int index = -1;
	gtk_tree_model_get(GTK_TREE_MODEL(_functionStore), &iter, FunctionIndex, &index, -1);
	EditorDocument *document = currentDocument();
	if (index < 0 || static_cast<std::size_t>(index) >= _functions.size() || !document || document != _functionDocument)
	{
		refreshFunctionList();
		return;
	}
	const auto &entry = _functions[static_cast<std::size_t>(index)];
	const sptr_t length = document->send(SCI_GETLENGTH);
	const sptr_t start = std::min<sptr_t>(static_cast<sptr_t>(entry.position), length);
	const sptr_t end = std::min<sptr_t>(start + static_cast<sptr_t>(entry.name.size()), length);
	document->send(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(document->send(SCI_LINEFROMPOSITION, start)));
	document->send(SCI_SETSEL, start, end);
	document->send(SCI_SCROLLCARET);
	if (!_selfTesting)
		gtk_widget_grab_focus(document->widget());
}

void EditorWindow::syncDocumentMap()
{
	if (!_mapView || !_settings.showDocumentMap)
		return;
	EditorDocument *document = currentDocument();
	if (!document)
	{
		if (_mapDocumentPointer)
		{
			_mapView->send(SCI_SETDOCPOINTER, 0, 0);
			_mapDocumentPointer = 0;
		}
		return;
	}
	const sptr_t pointer = document->send(SCI_GETDOCPOINTER);
	if (pointer != _mapDocumentPointer)
	{
		// The map is a second, non-editable view of the same Scintilla document.
		_mapView->send(SCI_SETDOCPOINTER, 0, pointer);
		_mapDocumentPointer = pointer;
		_mapLanguage = nullptr;
	}
	if (_mapLanguage != document->language)
	{
		EditorDocument &map = *_mapView;
		applyStyles(map, *document->language, document);
		map.send(SCI_SETZOOM, static_cast<uptr_t>(-10));
		for (int margin = 0; margin < 5; ++margin)
			map.send(SCI_SETMARGINWIDTHN, static_cast<uptr_t>(margin), 0);
		map.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
		map.send(SCI_SETVSCROLLBAR, 0);
		map.send(SCI_SETHSCROLLBAR, 0);
		map.send(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
		map.send(SCI_SETCARETLINEVISIBLE, 0);
		map.send(SCI_SETVIEWWS, SCWS_INVISIBLE);
		map.send(SCI_SETVIEWEOL, 0);
		map.send(SCI_SETSELFORE, 0, 0);
		const sptr_t highlight = _settings.darkTheme ? colour(120, 160, 220) : colour(60, 110, 200);
		map.send(SCI_SETSELECTIONLAYER, SC_LAYER_OVER_TEXT);
		map.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, highlight | (static_cast<sptr_t>(0x50) << 24));
		map.send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, highlight | (static_cast<sptr_t>(0x50) << 24));
		map.send(SCI_SETCURSOR, SC_CURSORARROW);
		_mapLanguage = document->language;
	}
	const sptr_t lineCount = std::max<sptr_t>(document->send(SCI_GETLINECOUNT), 1);
	const sptr_t firstVisible = document->send(SCI_GETFIRSTVISIBLELINE);
	const sptr_t onScreen = std::max<sptr_t>(document->send(SCI_LINESONSCREEN), 1);
	const sptr_t firstLine = std::min(document->send(SCI_DOCLINEFROMVISIBLE, firstVisible), lineCount - 1);
	const sptr_t lastLine = std::min(document->send(SCI_DOCLINEFROMVISIBLE, firstVisible + onScreen - 1), lineCount - 1);
	_mapView->send(SCI_SETSEL, document->send(SCI_POSITIONFROMLINE, firstLine), document->send(SCI_GETLINEENDPOSITION, lastLine));
	const sptr_t mapLines = std::max<sptr_t>(_mapView->send(SCI_LINESONSCREEN), 1);
	const sptr_t center = (firstLine + lastLine) / 2;
	_mapView->send(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, center - mapLines / 2));
}

void EditorWindow::documentMapJumpToLine(sptr_t line)
{
	EditorDocument *document = currentDocument();
	if (!document)
		return;
	line = std::clamp<sptr_t>(line, 0, std::max<sptr_t>(document->send(SCI_GETLINECOUNT) - 1, 0));
	// Finishes any pending line wrapping so display lines match the document.
	document->send(SCI_ENSUREVISIBLE, static_cast<uptr_t>(line));
	const sptr_t visible = document->send(SCI_VISIBLEFROMDOCLINE, line);
	const sptr_t onScreen = std::max<sptr_t>(document->send(SCI_LINESONSCREEN), 1);
	document->send(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, visible - onScreen / 2));
	syncDocumentMap();
}

void EditorWindow::documentMapJumpToPoint(double x, double y)
{
	if (!_mapView)
		return;
	const sptr_t position = _mapView->send(SCI_POSITIONFROMPOINT, static_cast<uptr_t>(std::max(0.0, x)), static_cast<sptr_t>(std::max(0.0, y)));
	documentMapJumpToLine(_mapView->send(SCI_LINEFROMPOSITION, position));
}

void EditorWindow::documentMapScroll(int lines)
{
	if (EditorDocument *document = currentDocument())
	{
		document->send(SCI_LINESCROLL, 0, lines);
		syncDocumentMap();
	}
}

void EditorWindow::rebuildWorkspace()
{
	if (!_workspaceStore)
		return;
	gtk_tree_store_clear(_workspaceStore);
	GtkTreeIter iter;
	if (_settings.workspaceFolders.empty())
	{
		gtk_tree_store_append(_workspaceStore, &iter, nullptr);
		gtk_tree_store_set(_workspaceStore, &iter, WorkspaceName, "Clique em + para adicionar uma pasta", WorkspacePath, "",
			WorkspaceDirectory, FALSE, WorkspaceLoaded, TRUE, WorkspaceIcon, "list-add", -1);
		return;
	}
	for (const auto &folder : _settings.workspaceFolders)
	{
		const std::filesystem::path path = folder;
		const std::string name = path.filename().empty() ? folder : path.filename().string();
		gtk_tree_store_append(_workspaceStore, &iter, nullptr);
		gtk_tree_store_set(_workspaceStore, &iter, WorkspaceName, name.c_str(), WorkspacePath, folder.c_str(),
			WorkspaceDirectory, TRUE, WorkspaceLoaded, FALSE, WorkspaceIcon, "folder", -1);
		GtkTreeIter placeholder;
		gtk_tree_store_append(_workspaceStore, &placeholder, &iter);
		gtk_tree_store_set(_workspaceStore, &placeholder, WorkspaceName, "…", WorkspacePath, "", WorkspaceDirectory, FALSE, WorkspaceLoaded, TRUE, -1);
	}
}

void EditorWindow::populateWorkspaceNode(GtkTreeIter *iter)
{
	gboolean directory = FALSE;
	gboolean loaded = TRUE;
	gchar *pathText = nullptr;
	gtk_tree_model_get(GTK_TREE_MODEL(_workspaceStore), iter, WorkspaceDirectory, &directory, WorkspaceLoaded, &loaded, WorkspacePath, &pathText, -1);
	const std::string path = pathText ? pathText : "";
	g_free(pathText);
	if (!directory || loaded || path.empty())
		return;
	GtkTreeIter child;
	while (gtk_tree_model_iter_children(GTK_TREE_MODEL(_workspaceStore), &child, iter))
		gtk_tree_store_remove(_workspaceStore, &child);
	std::string error;
	const auto entries = listDirectory(path, false, error);
	for (const auto &entry : entries)
	{
		gtk_tree_store_append(_workspaceStore, &child, iter);
		gtk_tree_store_set(_workspaceStore, &child, WorkspaceName, entry.name.c_str(), WorkspacePath, entry.path.c_str(),
			WorkspaceDirectory, entry.directory, WorkspaceLoaded, !entry.directory, WorkspaceIcon, entry.directory ? "folder" : "text-x-generic", -1);
		if (entry.directory)
		{
			GtkTreeIter placeholder;
			gtk_tree_store_append(_workspaceStore, &placeholder, &child);
			gtk_tree_store_set(_workspaceStore, &placeholder, WorkspaceName, "…", WorkspacePath, "", WorkspaceDirectory, FALSE, WorkspaceLoaded, TRUE, -1);
		}
	}
	if (entries.empty())
	{
		const std::string message = error.empty() ? "(pasta vazia)" : "(não foi possível abrir: " + error + ")";
		gtk_tree_store_append(_workspaceStore, &child, iter);
		gtk_tree_store_set(_workspaceStore, &child, WorkspaceName, message.c_str(), WorkspacePath, "", WorkspaceDirectory, FALSE, WorkspaceLoaded, TRUE, -1);
	}
	gtk_tree_store_set(_workspaceStore, iter, WorkspaceLoaded, TRUE, -1);
}

void EditorWindow::activateWorkspaceRow(GtkTreePath *path)
{
	GtkTreeIter iter;
	if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(_workspaceStore), &iter, path))
		return;
	gboolean directory = FALSE;
	gchar *pathText = nullptr;
	gtk_tree_model_get(GTK_TREE_MODEL(_workspaceStore), &iter, WorkspaceDirectory, &directory, WorkspacePath, &pathText, -1);
	const std::string filePath = pathText ? pathText : "";
	g_free(pathText);
	if (filePath.empty())
	{
		if (_settings.workspaceFolders.empty())
			chooseWorkspaceFolder();
		return;
	}
	if (directory)
	{
		if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(_workspaceView), path))
			gtk_tree_view_collapse_row(GTK_TREE_VIEW(_workspaceView), path);
		else
			gtk_tree_view_expand_row(GTK_TREE_VIEW(_workspaceView), path, FALSE);
		return;
	}
	openPath(filePath);
}

std::string EditorWindow::selectedWorkspacePath(bool directoryOnly) const
{
	GtkTreeModel *model = nullptr;
	GtkTreeIter iter;
	if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(_workspaceView)), &model, &iter))
		return {};
	gboolean directory = FALSE;
	gchar *pathText = nullptr;
	gtk_tree_model_get(model, &iter, WorkspaceDirectory, &directory, WorkspacePath, &pathText, -1);
	std::string path = pathText ? pathText : "";
	g_free(pathText);
	if (!path.empty() && directoryOnly && !directory)
		path = std::filesystem::path(path).parent_path().string();
	return path;
}

bool EditorWindow::addWorkspaceFolder(const std::filesystem::path &folder)
{
	std::error_code error;
	const std::filesystem::path absolute = std::filesystem::absolute(folder, error).lexically_normal();
	if (error || !std::filesystem::is_directory(absolute, error))
	{
		showError("A pasta não existe ou não pode ser aberta: " + folder.string());
		return false;
	}
	std::string normal = absolute.string();
	while (normal.size() > 1 && normal.back() == '/')
		normal.pop_back();
	if (std::find(_settings.workspaceFolders.begin(), _settings.workspaceFolders.end(), normal) == _settings.workspaceFolders.end())
		_settings.workspaceFolders.push_back(normal);
	_settings.showWorkspace = true;
	rebuildWorkspace();
	applySettings();
	saveSettings();
	return true;
}

void EditorWindow::removeSelectedWorkspaceFolder()
{
	GtkTreeModel *model = nullptr;
	GtkTreeIter iter;
	if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(_workspaceView)), &model, &iter))
	{
		gtk_label_set_text(GTK_LABEL(_messageLabel), "Selecione uma pasta do espaço de trabalho para removê-la.");
		return;
	}
	GtkTreeIter parent;
	while (gtk_tree_model_iter_parent(model, &parent, &iter))
		iter = parent;
	gchar *pathText = nullptr;
	gtk_tree_model_get(model, &iter, WorkspacePath, &pathText, -1);
	const std::string root = pathText ? pathText : "";
	g_free(pathText);
	auto &folders = _settings.workspaceFolders;
	folders.erase(std::remove(folders.begin(), folders.end(), root), folders.end());
	rebuildWorkspace();
	saveSettings();
}

void EditorWindow::findInSelectedWorkspaceFolder()
{
	std::string folder = selectedWorkspacePath(true);
	if (folder.empty() && !_settings.workspaceFolders.empty())
		folder = _settings.workspaceFolders.front();
	showFindInFiles();
	if (!folder.empty())
		gtk_entry_set_text(GTK_ENTRY(_findFolder), folder.c_str());
}

void EditorWindow::chooseWorkspaceFolder()
{
	if (_selfTesting)
		return;
	GtkWidget *chooser = gtk_file_chooser_dialog_new("Adicionar pasta ao espaço de trabalho", GTK_WINDOW(_window),
		GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancelar", GTK_RESPONSE_CANCEL, "_Adicionar", GTK_RESPONSE_ACCEPT, nullptr);
	if (const EditorDocument *document = currentDocument(); document && !document->path.empty())
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), document->path.parent_path().c_str());
	if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
	{
		if (char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)))
		{
			addWorkspaceFolder(folder);
			g_free(folder);
		}
	}
	gtk_widget_destroy(chooser);
}

bool EditorWindow::openWorkspaceFile(const std::filesystem::path &file)
{
	std::string error;
	const auto folders = loadWorkspaceFile(file, error);
	if (!error.empty())
	{
		showError(error);
		return false;
	}
	_settings.workspaceFolders = folders;
	_settings.showWorkspace = true;
	rebuildWorkspace();
	applySettings();
	saveSettings();
	gtk_label_set_text(GTK_LABEL(_messageLabel), ("Espaço de trabalho aberto: " + file.filename().string()).c_str());
	return true;
}

bool EditorWindow::saveWorkspaceFile(const std::filesystem::path &file)
{
	std::string error;
	if (!NppLinux::saveWorkspaceFile(file, _settings.workspaceFolders, error))
	{
		showError("Não foi possível salvar o espaço de trabalho.\n\n" + error);
		return false;
	}
	gtk_label_set_text(GTK_LABEL(_messageLabel), ("Espaço de trabalho salvo: " + file.filename().string()).c_str());
	return true;
}

void EditorWindow::restartBackupTimer()
{
	if (_backupTimer)
	{
		g_source_remove(_backupTimer);
		_backupTimer = 0;
	}
	if (!_selfTesting && _settings.backupEnabled)
		_backupTimer = g_timeout_add_seconds(static_cast<guint>(std::clamp(_settings.backupInterval, 5, 3600)), backupTimer, this);
}

void EditorWindow::styleNeeded(EditorDocument *document, sptr_t end)
{
	if (!document)
		return;
	const sptr_t start = document->send(SCI_GETENDSTYLED);
	if (const UserLanguage *language = userLanguageFor(document->language))
	{
		styleUserLanguage(*document, *language, static_cast<long>(start), static_cast<long>(end));
		return;
	}
	if (end > start)
	{
		document->send(SCI_STARTSTYLING, start, 0);
		document->send(SCI_SETSTYLING, end - start, 0);
	}
}

void EditorWindow::charAdded(EditorDocument *document, int character)
{
	if (!document || _recordingMacro || _playingMacro)
		return;
	const int eol = static_cast<int>(document->send(SCI_GETEOLMODE));
	const bool newline = character == '\n' || (character == '\r' && eol == SC_EOL_CR);
	if (newline && _settings.autoIndent)
	{
		const sptr_t line = document->send(SCI_LINEFROMPOSITION, document->send(SCI_GETCURRENTPOS));
		if (line > 0)
		{
			const sptr_t indent = document->send(SCI_GETLINEINDENTATION, static_cast<uptr_t>(line - 1));
			if (indent > 0)
			{
				document->send(SCI_SETLINEINDENTATION, static_cast<uptr_t>(line), indent);
				document->send(SCI_GOTOPOS, document->send(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line)));
			}
		}
	}
	if (_settings.autoCloseBrackets && (character == '(' || character == '[' || character == '{'))
	{
		const sptr_t position = document->send(SCI_GETCURRENTPOS);
		const int next = position < document->send(SCI_GETLENGTH) ? static_cast<int>(document->send(SCI_GETCHARAT, static_cast<uptr_t>(position))) : 0;
		if (next == 0 || next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == ')' || next == ']' || next == '}' || next == ';' || next == ',')
		{
			const char closing[2] = {character == '(' ? ')' : character == '[' ? ']' : '}', '\0'};
			document->send(SCI_INSERTTEXT, static_cast<uptr_t>(position), reinterpret_cast<sptr_t>(closing));
		}
	}
}

void EditorWindow::updateBraceMatch(EditorDocument &document)
{
	if (!_settings.braceMatching)
	{
		document.send(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(-1), -1);
		return;
	}
	auto isBrace = [](int c) { return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}'; };
	const sptr_t caret = document.send(SCI_GETCURRENTPOS);
	sptr_t brace = -1;
	if (caret > 0 && isBrace(static_cast<int>(document.send(SCI_GETCHARAT, static_cast<uptr_t>(caret - 1)))))
		brace = caret - 1;
	else if (caret < document.send(SCI_GETLENGTH) && isBrace(static_cast<int>(document.send(SCI_GETCHARAT, static_cast<uptr_t>(caret)))))
		brace = caret;
	if (brace < 0)
	{
		document.send(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(-1), -1);
		return;
	}
	const sptr_t match = document.send(SCI_BRACEMATCH, static_cast<uptr_t>(brace), 0);
	if (match < 0)
		document.send(SCI_BRACEBADLIGHT, static_cast<uptr_t>(brace));
	else
		document.send(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(brace), match);
}

const UserLanguage *EditorWindow::userLanguageFor(const Language *language) const
{
	if (!language || language->id.rfind("udl:", 0) != 0)
		return nullptr;
	for (const auto &user : _userLanguages)
	{
		if ("udl:" + user.name == language->id)
			return &user;
	}
	return nullptr;
}

const Language &EditorWindow::languageFor(const std::filesystem::path &path) const
{
	std::string extension = path.extension().string();
	if (!extension.empty())
		extension.erase(0, 1);
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const std::string fileName = path.filename().string();
	for (const auto &entry : _userLanguageEntries)
	{
		for (const auto &candidate : entry.extensions)
		{
			if ((!extension.empty() && candidate == extension) || candidate == fileName)
				return entry;
		}
	}
	return languageForPath(path);
}

const Language *EditorWindow::languageById(const std::string &id) const
{
	if (id.empty())
		return nullptr;
	for (const auto &language : languages())
	{
		if (language.id == id)
			return &language;
	}
	for (const auto &entry : _userLanguageEntries)
	{
		if (entry.id == id && userLanguageFor(&entry))
			return &entry;
	}
	return nullptr;
}

std::filesystem::path EditorWindow::userLanguageDirectory() const
{
	return _settingsStore.path().parent_path() / "userDefineLangs";
}

void EditorWindow::reloadUserLanguages()
{
	std::vector<std::string> errors;
	_userLanguages = loadUserLanguages(userLanguageDirectory(), errors);
	for (auto &entry : _userLanguageEntries)
		entry.extensions.clear();
	for (const auto &user : _userLanguages)
	{
		const std::string id = "udl:" + user.name;
		auto found = std::find_if(_userLanguageEntries.begin(), _userLanguageEntries.end(), [&](const Language &entry) { return entry.id == id; });
		if (found == _userLanguageEntries.end())
		{
			_userLanguageEntries.emplace_back();
			found = std::prev(_userLanguageEntries.end());
		}
		found->id = id;
		found->name = user.name + " (usuário)";
		found->lexer.clear();
		found->extensions = user.extensions;
		found->keywords.clear();
		found->keywordStyles = {UserKeyword1, UserKeyword1 + 1};
		found->commentStyles = {UserComment, UserLineComment};
		found->stringStyles = {UserDelimiter1, UserDelimiter1 + 1, UserDelimiter1 + 2, UserDelimiter1 + 3, UserDelimiter1 + 4, UserDelimiter1 + 5, UserDelimiter1 + 6, UserDelimiter1 + 7};
		found->numberStyles = {UserNumber};
		found->operatorStyles = {UserOperator};
	}
	for (auto &document : _documents)
	{
		if (document->language->id.rfind("udl:", 0) != 0)
			continue;
		if (!userLanguageFor(document->language))
			document->language = &plainTextLanguage();
		applyTheme(*document);
	}
	_mapLanguage = nullptr;
	rebuildLanguageMenu();
	if (!errors.empty())
		setStatusMessage("Algumas linguagens do usuário não foram carregadas: " + errors.front());
}

std::size_t EditorWindow::importUserLanguage(const std::filesystem::path &file, std::string &error)
{
	std::ifstream input(file, std::ios::binary);
	if (!input)
	{
		error = "Não foi possível abrir " + file.string();
		return 0;
	}
	std::ostringstream buffer;
	buffer << input.rdbuf();
	std::vector<UserLanguage> found;
	if (!parseUserLanguages(buffer.str(), found, error))
		return 0;
	std::size_t saved = 0;
	for (auto &language : found)
	{
		// Replace a definition with the same name instead of creating a duplicate.
		for (const auto &existing : _userLanguages)
		{
			if (existing.name == language.name)
				language.file = existing.file;
		}
		if (saveUserLanguage(userLanguageDirectory(), language, error))
			++saved;
	}
	reloadUserLanguages();
	return saved;
}

bool EditorWindow::saveUserLanguageDefinition(UserLanguage language, std::string &error)
{
	if (language.name.empty())
	{
		error = "Dê um nome à linguagem.";
		return false;
	}
	for (const auto &builtIn : languages())
	{
		if (builtIn.name == language.name)
		{
			error = "Já existe uma linguagem embutida com esse nome.";
			return false;
		}
	}
	if (!saveUserLanguage(userLanguageDirectory(), language, error))
		return false;
	reloadUserLanguages();
	return true;
}

bool EditorWindow::removeUserLanguage(const std::string &name)
{
	const auto found = std::find_if(_userLanguages.begin(), _userLanguages.end(), [&](const UserLanguage &language) { return language.name == name; });
	if (found == _userLanguages.end() || found->file.empty())
		return false;
	// The definition is moved aside, not deleted, so it can be restored by hand.
	const auto archive = userLanguageDirectory() / "removidas";
	std::error_code error;
	std::filesystem::create_directories(archive, error);
	std::filesystem::path target = archive / found->file.filename();
	for (int copy = 2; std::filesystem::exists(target, error); ++copy)
		target = archive / (found->file.stem().string() + "-" + std::to_string(copy) + ".xml");
	std::filesystem::rename(found->file, target, error);
	if (error)
		return false;
	reloadUserLanguages();
	return true;
}

void EditorWindow::rebuildLanguageMenu()
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(_languageMenu));
	for (GList *entry = children; entry; entry = entry->next)
		gtk_widget_destroy(GTK_WIDGET(entry->data));
	g_list_free(children);
	GSList *group = nullptr;
	auto add = [&](const Language &language) {
		GtkWidget *item = gtk_radio_menu_item_new_with_label(group, language.name.c_str());
		group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
		attachData(item, "npp-language", const_cast<Language *>(&language));
		attachData(item, "npp-window", this);
		g_signal_connect(item, "toggled", G_CALLBACK(languageActivated), item);
		gtk_menu_shell_append(GTK_MENU_SHELL(_languageMenu), item);
	};
	_updating = true;
	for (const auto &language : languages())
		add(language);
	bool separator = false;
	for (const auto &entry : _userLanguageEntries)
	{
		if (!userLanguageFor(&entry))
			continue;
		if (!separator)
		{
			gtk_menu_shell_append(GTK_MENU_SHELL(_languageMenu), gtk_separator_menu_item_new());
			separator = true;
		}
		add(entry);
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(_languageMenu), gtk_separator_menu_item_new());
	GtkWidget *userLanguages = gtk_menu_new();
	menuItem(userLanguages, "_Definir sua linguagem…", this, Action::ManageUserLanguages, nullptr);
	menuItem(userLanguages, "_Importar linguagem…", this, Action::ImportUserLanguage, nullptr);
	menuItem(userLanguages, "_Abrir pasta das linguagens do usuário", this, Action::OpenUserLanguageFolder, nullptr);
	submenu(_languageMenu, "Linguagens definidas pelo _usuário", userLanguages);
	gtk_widget_show_all(_languageMenu);
	_updating = false;
	updateLanguageMenu();
}

void EditorWindow::scanPlugins(const std::vector<std::filesystem::path> &directories)
{
	_plugins.scan(directories, _settings.enabledPlugins);
	_plugins.loadEnabled(&_pluginHost);
	rebuildPluginMenu();
}

bool EditorWindow::enablePlugin(const std::string &id, bool enabled)
{
	auto &plugins = _plugins.plugins();
	const auto plugin = std::find_if(plugins.begin(), plugins.end(), [&](const Plugin &candidate) { return candidate.id == id; });
	if (plugin == plugins.end())
		return false;
	auto &list = _settings.enabledPlugins;
	list.erase(std::remove(list.begin(), list.end(), id), list.end());
	plugin->enabled = enabled;
	bool ok = true;
	if (enabled)
	{
		list.push_back(id);
		plugin->error.clear();
		ok = _plugins.load(*plugin, &_pluginHost);
		setStatusMessage(ok ? "Plugin ativado: " + plugin->name : "O plugin não pôde ser carregado: " + plugin->error);
	}
	else if (plugin->loaded)
		setStatusMessage("O plugin será desativado na próxima vez que o editor for aberto.");
	saveSettings();
	rebuildPluginMenu();
	return ok;
}

bool EditorWindow::runPluginCommand(std::size_t pluginIndex, std::size_t commandIndex)
{
	auto &plugins = _plugins.plugins();
	if (pluginIndex >= plugins.size() || !plugins[pluginIndex].loaded || commandIndex >= plugins[pluginIndex].commands.size())
		return false;
	plugins[pluginIndex].commands[commandIndex].run(&_pluginHost);
	refreshDocument();
	return true;
}

void EditorWindow::rebuildPluginMenu()
{
	if (!_pluginMenu)
		return;
	GList *children = gtk_container_get_children(GTK_CONTAINER(_pluginMenu));
	for (GList *entry = children; entry; entry = entry->next)
		gtk_widget_destroy(GTK_WIDGET(entry->data));
	g_list_free(children);
	GtkAccelGroup *accelerators = GTK_ACCEL_GROUP(g_object_get_data(G_OBJECT(_window), "npp-accelerators"));
	const auto &plugins = _plugins.plugins();
	bool any = false;
	for (std::size_t i = 0; i < plugins.size(); ++i)
	{
		const Plugin &plugin = plugins[i];
		if (!plugin.loaded)
			continue;
		any = true;
		GtkWidget *menu = gtk_menu_new();
		for (std::size_t c = 0; c < plugin.commands.size(); ++c)
		{
			GtkWidget *item = gtk_menu_item_new_with_label(plugin.commands[c].name.c_str());
			attachData(item, "npp-plugin", GINT_TO_POINTER(static_cast<int>(i)));
			attachData(item, "npp-plugin-command", GINT_TO_POINTER(static_cast<int>(c)));
			g_signal_connect(item, "activate", G_CALLBACK(pluginCommandActivated), this);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
		}
		if (plugin.commands.empty())
		{
			GtkWidget *none = gtk_menu_item_new_with_label("(sem comandos)");
			gtk_widget_set_sensitive(none, FALSE);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), none);
		}
		GtkWidget *item = gtk_menu_item_new_with_label(plugin.name.c_str());
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
		gtk_menu_shell_append(GTK_MENU_SHELL(_pluginMenu), item);
	}
	if (!any)
	{
		GtkWidget *none = gtk_menu_item_new_with_label("Nenhum plugin ativado");
		gtk_widget_set_sensitive(none, FALSE);
		gtk_menu_shell_append(GTK_MENU_SHELL(_pluginMenu), none);
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(_pluginMenu), gtk_separator_menu_item_new());
	menuItem(_pluginMenu, "_Gerenciar plugins…", this, Action::ManagePlugins, accelerators);
	menuItem(_pluginMenu, "_Abrir pasta de plugins", this, Action::OpenPluginFolder, accelerators);
	// Built in: the features of the Windows plugins Compare, MIME Tools, Converter, NppExport and JSMin.
	gtk_menu_shell_append(GTK_MENU_SHELL(_pluginMenu), gtk_separator_menu_item_new());
	const auto altControl = static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_MOD1_MASK);
	GtkWidget *compare = gtk_menu_new();
	menuItem(compare, "_Comparar com outro documento…", this, Action::CompareWith, accelerators, GDK_KEY_c, altControl);
	menuItem(compare, "_Próxima diferença", this, Action::CompareNext, accelerators, GDK_KEY_Next, GDK_MOD1_MASK);
	menuItem(compare, "Diferença _anterior", this, Action::ComparePrevious, accelerators, GDK_KEY_Prior, GDK_MOD1_MASK);
	menuItem(compare, "_Limpar comparação", this, Action::CompareClear, accelerators, GDK_KEY_x, altControl);
	submenu(_pluginMenu, "_Comparar", compare);
	GtkWidget *mime = gtk_menu_new();
	menuItem(mime, "Codificar em _Base64", this, Action::Base64Encode, accelerators);
	menuItem(mime, "_Decodificar Base64", this, Action::Base64Decode, accelerators);
	menuItem(mime, "Codificar para _URL", this, Action::UrlEncode, accelerators);
	menuItem(mime, "Decodificar _URL", this, Action::UrlDecode, accelerators);
	submenu(_pluginMenu, "_MIME Tools", mime);
	GtkWidget *converter = gtk_menu_new();
	menuItem(converter, "_ASCII → Hex", this, Action::AsciiToHex, accelerators);
	menuItem(converter, "_Hex → ASCII", this, Action::HexToAscii, accelerators);
	submenu(_pluginMenu, "Con_versor", converter);
	GtkWidget *exporter = gtk_menu_new();
	menuItem(exporter, "Exportar para _HTML…", this, Action::ExportHtml, accelerators);
	menuItem(exporter, "Exportar para _RTF…", this, Action::ExportRtf, accelerators);
	submenu(_pluginMenu, "_Exportar com cores", exporter);
	GtkWidget *js = gtk_menu_new();
	menuItem(js, "_Minificar JavaScript (JSMin)", this, Action::JsMinify, accelerators);
	menuItem(js, "_Formatar JavaScript", this, Action::JsFormat, accelerators);
	submenu(_pluginMenu, "_JSMin", js);
	gtk_widget_show_all(_pluginMenu);
}

void EditorWindow::showMessage(const std::string &message)
{
	gtk_label_set_text(GTK_LABEL(_messageLabel), message.c_str());
	if (_selfTesting)
		return;
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(_window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", message.c_str());
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

void EditorWindow::setStatusMessage(const std::string &message)
{
	gtk_label_set_text(GTK_LABEL(_messageLabel), message.c_str());
}

std::string EditorWindow::commentPrefix(const EditorDocument &document) const
{
	if (const UserLanguage *language = userLanguageFor(document.language))
		return language->lineComments.empty() ? std::string() : language->lineComments.front();
	return lineCommentFor(document.language->id);
}

void EditorWindow::showPreferences()
{
	if (_selfTesting)
		return;
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Preferências", GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"_Cancelar", GTK_RESPONSE_CANCEL, "_Aplicar", GTK_RESPONSE_ACCEPT, nullptr);
	GtkWidget *tabs = gtk_notebook_new();
	gtk_container_set_border_width(GTK_CONTAINER(tabs), 8);
	auto page = [&](const char *title) {
		GtkWidget *grid = gtk_grid_new();
		gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
		gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
		gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
		gtk_notebook_append_page(GTK_NOTEBOOK(tabs), grid, gtk_label_new(title));
		return grid;
	};
	auto label = [](GtkWidget *grid, const char *text, int row) {
		GtkWidget *caption = gtk_label_new(text);
		gtk_label_set_xalign(GTK_LABEL(caption), 0.0);
		gtk_grid_attach(GTK_GRID(grid), caption, 0, row, 1, 1);
	};
	auto check = [](GtkWidget *grid, const char *text, bool active, int row) {
		GtkWidget *button = gtk_check_button_new_with_label(text);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);
		gtk_grid_attach(GTK_GRID(grid), button, 0, row, 2, 1);
		return button;
	};

	GtkWidget *editor = page("Editor");
	label(editor, "Fonte do editor:", 0);
	const std::string font = _settings.fontFamily + " " + std::to_string(_settings.fontSize);
	GtkWidget *fontButton = gtk_font_button_new_with_font(font.c_str());
	gtk_grid_attach(GTK_GRID(editor), fontButton, 1, 0, 1, 1);
	label(editor, "Largura da tabulação:", 1);
	GtkWidget *tabWidth = gtk_spin_button_new_with_range(1, 16, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(tabWidth), _settings.tabWidth);
	gtk_grid_attach(GTK_GRID(editor), tabWidth, 1, 1, 1, 1);
	label(editor, "Linha-guia na coluna (0 = desligada):", 2);
	GtkWidget *edge = gtk_spin_button_new_with_range(0, 500, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(edge), _settings.edgeColumn);
	gtk_grid_attach(GTK_GRID(editor), edge, 1, 2, 1, 1);
	GtkWidget *useTabs = check(editor, "Usar tabulações em vez de espaços", _settings.useTabs, 3);
	GtkWidget *autoIndent = check(editor, "Manter o recuo da linha anterior ao pressionar Enter", _settings.autoIndent, 4);
	GtkWidget *autoClose = check(editor, "Fechar automaticamente ( [ {", _settings.autoCloseBrackets, 5);
	GtkWidget *caretLine = check(editor, "Destacar a linha atual", _settings.highlightCaretLine, 6);
	GtkWidget *braces = check(editor, "Destacar o par de ( ) [ ] { }", _settings.braceMatching, 7);
	GtkWidget *guides = check(editor, "Mostrar guias de recuo", _settings.indentGuides, 8);

	GtkWidget *newDocument = page("Novo documento");
	label(newDocument, "Fim de linha:", 0);
	GtkWidget *eol = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(eol), "Unix (LF)");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(eol), "Windows (CRLF)");
	gtk_combo_box_set_active(GTK_COMBO_BOX(eol), _settings.defaultEolWindows ? 1 : 0);
	gtk_grid_attach(GTK_GRID(newDocument), eol, 1, 0, 1, 1);
	label(newDocument, "Linguagem:", 1);
	GtkWidget *language = gtk_combo_box_text_new();
	std::vector<std::string> languageIds;
	for (const auto &entry : languages())
	{
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(language), entry.name.c_str());
		languageIds.push_back(entry.id);
	}
	for (const auto &entry : _userLanguageEntries)
	{
		if (!userLanguageFor(&entry))
			continue;
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(language), entry.name.c_str());
		languageIds.push_back(entry.id);
	}
	const auto selected = std::find(languageIds.begin(), languageIds.end(), _settings.defaultLanguage.empty() ? "text" : _settings.defaultLanguage);
	gtk_combo_box_set_active(GTK_COMBO_BOX(language), selected == languageIds.end() ? 0 : static_cast<int>(selected - languageIds.begin()));
	gtk_grid_attach(GTK_GRID(newDocument), language, 1, 1, 1, 1);
	label(newDocument, "Codificação:", 2);
	GtkWidget *newEncoding = gtk_combo_box_text_new();
	int encodingIndex = 0;
	for (std::size_t i = 0; i < encodings().size(); ++i)
	{
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(newEncoding), encodings()[i].label);
		if (_settings.newDocumentEncoding == encodings()[i].id)
			encodingIndex = static_cast<int>(i);
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(newEncoding), encodingIndex);
	gtk_grid_attach(GTK_GRID(newDocument), newEncoding, 1, 2, 1, 1);

	GtkWidget *backup = page("Backup e sessão");
	GtkWidget *backupEnabled = check(backup, "Fazer backup automático dos documentos alterados", _settings.backupEnabled, 0);
	label(backup, "Intervalo do backup (segundos):", 1);
	GtkWidget *interval = gtk_spin_button_new_with_range(5, 3600, 5);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(interval), _settings.backupInterval);
	gtk_grid_attach(GTK_GRID(backup), interval, 1, 1, 1, 1);
	GtkWidget *restore = check(backup, "Restaurar arquivos abertos na próxima sessão", _settings.restoreSession, 2);
	label(backup, "Arquivos recentes no menu:", 3);
	GtkWidget *recent = gtk_spin_button_new_with_range(0, 30, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(recent), _settings.maxRecentFiles);
	gtk_grid_attach(GTK_GRID(backup), recent, 1, 3, 1, 1);
	GtkWidget *rememberUnsaved = check(backup,
		"Ao sair, guardar os textos não salvos e reabri-los na próxima vez, sem perguntar", _settings.rememberUnsaved, 4);
	GtkWidget *rememberNote = gtk_label_new("Igual ao “instantâneo de sessão” da versão Windows. Liga também “Restaurar arquivos abertos”.");
	gtk_label_set_xalign(GTK_LABEL(rememberNote), 0.0);
	gtk_label_set_line_wrap(GTK_LABEL(rememberNote), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(rememberNote), "dim-label");
	gtk_grid_attach(GTK_GRID(backup), rememberNote, 0, 5, 2, 1);
	// Keeping unsaved text needs the session, as in the Windows version.
	g_signal_connect(rememberUnsaved, "toggled", G_CALLBACK(+[](GtkToggleButton *button, gpointer other) {
		if (gtk_toggle_button_get_active(button))
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other), TRUE);
	}), restore);
	g_signal_connect(restore, "toggled", G_CALLBACK(+[](GtkToggleButton *button, gpointer other) {
		if (!gtk_toggle_button_get_active(button))
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other), FALSE);
	}), rememberUnsaved);

	GtkWidget *misc = page("Diversos");
	GtkWidget *bookmarkMargin = check(misc, "Mostrar a margem de marcadores e de alterações", _settings.showBookmarkMargin, 0);
	GtkWidget *foldMargin = check(misc, "Mostrar a margem para recolher blocos de código", _settings.showFoldMargin, 1);
	GtkWidget *changeHistory = check(misc, "Marcar linhas alteradas (vale para documentos abertos depois)", _settings.changeHistory, 2);
	label(misc, "Formato da data/hora personalizada:", 3);
	GtkWidget *dateFormat = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(dateFormat), _settings.dateTimeFormat.c_str());
	gtk_widget_set_tooltip_text(dateFormat, "%d dia, %m mês, %Y ano, %H hora, %M minuto, %S segundo");
	gtk_grid_attach(GTK_GRID(misc), dateFormat, 1, 3, 1, 1);
	label(misc, "Pesquisa na internet ($(TEXT) = texto):", 4);
	GtkWidget *autoDetect = check(misc, "Avisar quando outro programa alterar um arquivo aberto", _settings.fileAutoDetect, 5);
	GtkWidget *autoReload = check(misc, "Recarregar sem perguntar quando não houver alterações não salvas", _settings.autoReloadSilently, 6);
	GtkWidget *nppColors = check(misc, "Cores por linguagem como no Windows (palavras-chave, tipos, números, textos...)", _settings.nppLanguageColors, 7);
	GtkWidget *searchEngine = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(searchEngine), _settings.searchEngine.c_str());
	gtk_entry_set_width_chars(GTK_ENTRY(searchEngine), 40);
	gtk_grid_attach(GTK_GRID(misc), searchEngine, 1, 4, 1, 1);

	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), tabs);
	gtk_widget_show_all(dialog);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
	{
		PangoFontDescription *description = pango_font_description_from_string(gtk_font_button_get_font_name(GTK_FONT_BUTTON(fontButton)));
		const char *family = pango_font_description_get_family(description);
		_settings.fontFamily = family ? family : "Monospace";
		_settings.fontSize = std::clamp(pango_font_description_get_size(description) / PANGO_SCALE, 6, 72);
		pango_font_description_free(description);
		_settings.tabWidth = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(tabWidth));
		_settings.edgeColumn = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(edge));
		_settings.useTabs = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(useTabs));
		_settings.autoIndent = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(autoIndent));
		_settings.autoCloseBrackets = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(autoClose));
		_settings.highlightCaretLine = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(caretLine));
		_settings.braceMatching = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(braces));
		_settings.indentGuides = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(guides));
		_settings.defaultEolWindows = gtk_combo_box_get_active(GTK_COMBO_BOX(eol)) == 1;
		const int languageIndex = gtk_combo_box_get_active(GTK_COMBO_BOX(language));
		_settings.defaultLanguage = languageIndex > 0 && static_cast<std::size_t>(languageIndex) < languageIds.size() ? languageIds[static_cast<std::size_t>(languageIndex)] : std::string();
		_settings.backupEnabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(backupEnabled));
		_settings.backupInterval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(interval));
		_settings.restoreSession = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restore));
		_settings.maxRecentFiles = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(recent));
		_settings.rememberUnsaved = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rememberUnsaved));
		if (_settings.rememberUnsaved)
			_settings.restoreSession = true;
		_sessionWritable = _settings.restoreSession;
		const int chosenEncoding = gtk_combo_box_get_active(GTK_COMBO_BOX(newEncoding));
		if (chosenEncoding >= 0 && static_cast<std::size_t>(chosenEncoding) < encodings().size())
			_settings.newDocumentEncoding = encodings()[static_cast<std::size_t>(chosenEncoding)].id;
		_settings.showBookmarkMargin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bookmarkMargin));
		_settings.fileAutoDetect = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(autoDetect));
		_settings.nppLanguageColors = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(nppColors));
		_settings.autoReloadSilently = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(autoReload));
		_settings.showFoldMargin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(foldMargin));
		_settings.changeHistory = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(changeHistory));
		const std::string format = gtk_entry_get_text(GTK_ENTRY(dateFormat));
		if (!format.empty())
			_settings.dateTimeFormat = format;
		const std::string engine = gtk_entry_get_text(GTK_ENTRY(searchEngine));
		if (engine.rfind("https://", 0) == 0 || engine.rfind("http://", 0) == 0)
			_settings.searchEngine = engine;
		if (_settings.recentFiles.size() > static_cast<std::size_t>(_settings.maxRecentFiles))
			_settings.recentFiles.resize(static_cast<std::size_t>(_settings.maxRecentFiles));
		rebuildRecentMenu();
		restartBackupTimer();
		applySettings();
		saveSettings();
	}
	gtk_widget_destroy(dialog);
}

void EditorWindow::showStyleConfigurator()
{
	if (_selfTesting)
		return;
	StyleDialog state;
	state.dark = _settings.darkTheme;
	state.theme = _theme;
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Configurar estilos e cores", GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"_Restaurar padrão", 1, "_Cancelar", GTK_RESPONSE_CANCEL, "_Aplicar", GTK_RESPONSE_ACCEPT, nullptr);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	const std::string themeName = std::string("Tema em edição: ") + (state.dark ? "escuro" : "claro") + " (troque em Exibir → Tema escuro)";
	GtkWidget *note = gtk_label_new(themeName.c_str());
	gtk_label_set_xalign(GTK_LABEL(note), 0.0);
	gtk_grid_attach(GTK_GRID(grid), note, 0, 0, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Elemento:"), 0, 1, 1, 1);
	state.items = gtk_combo_box_text_new();
	for (std::size_t i = 0; i < themeItemCount; ++i)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state.items), themeItemLabel(static_cast<ThemeItem>(i)));
	gtk_grid_attach(GTK_GRID(grid), state.items, 1, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Cor do texto:"), 0, 2, 1, 1);
	state.fore = gtk_color_button_new();
	gtk_grid_attach(GTK_GRID(grid), state.fore, 1, 2, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Cor de fundo:"), 0, 3, 1, 1);
	state.back = gtk_color_button_new();
	gtk_grid_attach(GTK_GRID(grid), state.back, 1, 3, 1, 1);
	state.bold = gtk_check_button_new_with_label("Negrito");
	state.italic = gtk_check_button_new_with_label("Itálico");
	gtk_grid_attach(GTK_GRID(grid), state.bold, 1, 4, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), state.italic, 1, 5, 1, 1);
	g_signal_connect(state.items, "changed", G_CALLBACK(styleDialogItemChanged), &state);
	g_signal_connect(state.fore, "color-set", G_CALLBACK(styleDialogStore), &state);
	g_signal_connect(state.back, "color-set", G_CALLBACK(styleDialogStore), &state);
	g_signal_connect(state.bold, "toggled", G_CALLBACK(styleDialogStore), &state);
	g_signal_connect(state.italic, "toggled", G_CALLBACK(styleDialogStore), &state);
	gtk_combo_box_set_active(GTK_COMBO_BOX(state.items), 0);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid);
	gtk_widget_show_all(dialog);
	int response;
	while ((response = gtk_dialog_run(GTK_DIALOG(dialog))) == 1)
	{
		state.theme = defaultTheme(state.dark);
		styleDialogLoad(&state);
	}
	if (response == GTK_RESPONSE_ACCEPT)
	{
		const std::string serialised = serialiseTheme(state.theme);
		const std::string stored = serialised == serialiseTheme(defaultTheme(state.dark)) ? std::string() : serialised;
		(state.dark ? _settings.darkStyles : _settings.lightStyles) = stored;
		applySettings();
		saveSettings();
	}
	gtk_widget_destroy(dialog);
}

void EditorWindow::showUserLanguageDialog()
{
	if (_selfTesting)
		return;
	LanguageDialog state;
	state.window = this;
	state.languages = _userLanguages;
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Linguagens definidas pelo usuário", GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"_Importar…", 3, "_Exportar…", 4, "_Remover", 2, "_Fechar", GTK_RESPONSE_CLOSE, "_Salvar", 1, nullptr);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
	int row = 0;
	auto field = [&](const char *caption, const char *tooltip = nullptr) {
		GtkWidget *label = gtk_label_new(caption);
		gtk_label_set_xalign(GTK_LABEL(label), 1.0);
		gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
		GtkWidget *entry = gtk_entry_new();
		gtk_widget_set_hexpand(entry, TRUE);
		if (tooltip)
			gtk_widget_set_tooltip_text(entry, tooltip);
		gtk_grid_attach(GTK_GRID(grid), entry, 1, row++, 1, 1);
		return entry;
	};
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Linguagem:"), 0, row, 1, 1);
	state.selector = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state.selector), "(nova linguagem)");
	for (const auto &language : state.languages)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state.selector), language.name.c_str());
	gtk_grid_attach(GTK_GRID(grid), state.selector, 1, row++, 1, 1);
	state.name = field("Nome:");
	state.extensions = field("Extensões:", "Separadas por espaço, sem ponto. Ex.: conf cfg");
	state.lineComment = field("Comentário de linha:", "Ex.: // ou #");
	state.blockOpen = field("Início de comentário em bloco:", "Ex.: /*");
	state.blockClose = field("Fim de comentário em bloco:", "Ex.: */");
	for (int i = 0; i < 4; ++i)
	{
		const std::string caption = "Palavras-chave " + std::to_string(i + 1) + ":";
		state.keywords[i] = field(caption.c_str(), "Palavras separadas por espaço");
	}
	state.operators = field("Operadores:", "Símbolos separados por espaço. Ex.: + - * / = ==");
	state.stringOpen = field("Delimitador de texto:", "Ex.: \" (o mesmo símbolo abre e fecha)");
	state.stringEscape = field("Caractere de escape:", "Ex.: \\");
	state.caseIgnored = gtk_check_button_new_with_label("Não diferenciar maiúsculas nas palavras-chave");
	gtk_grid_attach(GTK_GRID(grid), state.caseIgnored, 1, row++, 1, 1);
	GtkWidget *note = gtk_label_new("Os arquivos usam o formato XML de linguagens do Notepad++, então definições exportadas do Notepad++ para Windows podem ser importadas.");
	gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(note), 60);
	gtk_grid_attach(GTK_GRID(grid), note, 0, row++, 2, 1);
	g_signal_connect(state.selector, "changed", G_CALLBACK(languageDialogLoad), &state);
	gtk_combo_box_set_active(GTK_COMBO_BOX(state.selector), 0);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid);
	gtk_widget_show_all(dialog);
	auto refreshSelector = [&](const std::string &select) {
		state.languages = _userLanguages;
		g_signal_handlers_block_by_func(state.selector, reinterpret_cast<gpointer>(languageDialogLoad), &state);
		gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(state.selector));
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state.selector), "(nova linguagem)");
		int active = 0;
		for (std::size_t i = 0; i < state.languages.size(); ++i)
		{
			gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state.selector), state.languages[i].name.c_str());
			if (state.languages[i].name == select)
				active = static_cast<int>(i + 1);
		}
		g_signal_handlers_unblock_by_func(state.selector, reinterpret_cast<gpointer>(languageDialogLoad), &state);
		gtk_combo_box_set_active(GTK_COMBO_BOX(state.selector), active);
		languageDialogLoad(GTK_COMBO_BOX(state.selector), &state);
	};
	for (;;)
	{
		const int response = gtk_dialog_run(GTK_DIALOG(dialog));
		const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(state.selector));
		const UserLanguage *current = index > 0 && static_cast<std::size_t>(index) <= state.languages.size() ? &state.languages[static_cast<std::size_t>(index - 1)] : nullptr;
		if (response == 1)
		{
			UserLanguage language = current ? *current : UserLanguage();
			language.name = gtk_entry_get_text(GTK_ENTRY(state.name));
			if (current && language.name != current->name)
				language.file.clear();
			language.extensions = splitWords(gtk_entry_get_text(GTK_ENTRY(state.extensions)));
			const std::string comment = gtk_entry_get_text(GTK_ENTRY(state.lineComment));
			if (language.lineComments.empty())
				language.lineComments.push_back(comment);
			else
				language.lineComments.front() = comment;
			if (comment.empty())
				language.lineComments.erase(language.lineComments.begin());
			language.blockOpen = gtk_entry_get_text(GTK_ENTRY(state.blockOpen));
			language.blockClose = gtk_entry_get_text(GTK_ENTRY(state.blockClose));
			for (int i = 0; i < 4; ++i)
				language.keywords[static_cast<std::size_t>(i)] = splitWords(gtk_entry_get_text(GTK_ENTRY(state.keywords[i])));
			language.operators = splitWords(gtk_entry_get_text(GTK_ENTRY(state.operators)));
			language.delimiters[0].open = gtk_entry_get_text(GTK_ENTRY(state.stringOpen));
			language.delimiters[0].close = language.delimiters[0].open;
			language.delimiters[0].escape = gtk_entry_get_text(GTK_ENTRY(state.stringEscape));
			language.caseIgnored = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state.caseIgnored));
			std::string error;
			if (saveUserLanguageDefinition(language, error))
			{
				refreshSelector(language.name);
				setStatusMessage("Linguagem salva: " + language.name);
			}
			else
				showError(error);
		}
		else if (response == 2 && current)
		{
			const std::string name = current->name;
			if (removeUserLanguage(name))
			{
				refreshSelector("");
				setStatusMessage("Linguagem removida. O arquivo foi guardado na pasta userDefineLangs/removidas.");
			}
		}
		else if (response == 3)
		{
			GtkWidget *chooser = gtk_file_chooser_dialog_new("Importar linguagem do usuário", GTK_WINDOW(dialog), GTK_FILE_CHOOSER_ACTION_OPEN,
				"_Cancelar", GTK_RESPONSE_CANCEL, "_Importar", GTK_RESPONSE_ACCEPT, nullptr);
			GtkFileFilter *filter = gtk_file_filter_new();
			gtk_file_filter_set_name(filter, "Linguagens do Notepad++ (*.xml)");
			gtk_file_filter_add_pattern(filter, "*.xml");
			gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
			if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
			{
				if (char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)))
				{
					std::string error;
					const std::size_t count = importUserLanguage(file, error);
					g_free(file);
					if (count)
						refreshSelector(_userLanguages.empty() ? "" : _userLanguages.back().name);
					else
						showError("Não foi possível importar.\n\n" + error);
				}
			}
			gtk_widget_destroy(chooser);
		}
		else if (response == 4 && current)
		{
			GtkWidget *chooser = gtk_file_chooser_dialog_new("Exportar linguagem do usuário", GTK_WINDOW(dialog), GTK_FILE_CHOOSER_ACTION_SAVE,
				"_Cancelar", GTK_RESPONSE_CANCEL, "_Exportar", GTK_RESPONSE_ACCEPT, nullptr);
			gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
			gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), userLanguageFileName(current->name).c_str());
			if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
			{
				if (char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)))
				{
					std::ofstream output(file, std::ios::binary | std::ios::trunc);
					output << userLanguageToXml(*current);
					setStatusMessage(output ? "Linguagem exportada." : "Não foi possível exportar a linguagem.");
					g_free(file);
				}
			}
			gtk_widget_destroy(chooser);
		}
		else if (response != 2 && response != 4)
			break;
	}
	gtk_widget_destroy(dialog);
}

void EditorWindow::showPluginManager()
{
	if (_selfTesting)
		return;
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Gerenciar plugins", GTK_WINDOW(_window), GTK_DIALOG_MODAL,
		"Abrir _pasta de plugins", 1, "_Procurar novamente", 2, "_Fechar", GTK_RESPONSE_CLOSE, nullptr);
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(content), 14);
	GtkWidget *warning = gtk_label_new(
		"Plugins são programas que rodam dentro do editor com as suas permissões. Ative apenas plugins de fontes confiáveis.\n"
		"Plugins DLL do Windows não funcionam: é preciso uma versão compilada para Linux.");
	gtk_label_set_line_wrap(GTK_LABEL(warning), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(warning), 70);
	gtk_label_set_xalign(GTK_LABEL(warning), 0.0);
	gtk_box_pack_start(GTK_BOX(content), warning, FALSE, FALSE, 0);
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_widget_set_size_request(scroller, 560, 260);
	gtk_container_add(GTK_CONTAINER(scroller), list);
	gtk_box_pack_start(GTK_BOX(content), scroller, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), content);
	std::vector<std::unique_ptr<PluginRow>> rows;
	auto fill = [&]() {
		GList *children = gtk_container_get_children(GTK_CONTAINER(list));
		for (GList *entry = children; entry; entry = entry->next)
			gtk_widget_destroy(GTK_WIDGET(entry->data));
		g_list_free(children);
		rows.clear();
		if (_plugins.plugins().empty())
		{
			std::string text = "Nenhum plugin encontrado. Copie arquivos .so para:\n";
			text += PluginManager::defaultDirectories().front().string();
			GtkWidget *empty = gtk_label_new(text.c_str());
			gtk_label_set_selectable(GTK_LABEL(empty), TRUE);
			gtk_box_pack_start(GTK_BOX(list), empty, FALSE, FALSE, 0);
		}
		for (const auto &plugin : _plugins.plugins())
		{
			GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
			const std::string title = plugin.name + (plugin.version.empty() ? "" : " " + plugin.version);
			GtkWidget *toggle = gtk_check_button_new_with_label(title.c_str());
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), plugin.enabled);
			rows.push_back(std::make_unique<PluginRow>(PluginRow{this, plugin.id}));
			g_signal_connect(toggle, "toggled", G_CALLBACK(pluginToggled), rows.back().get());
			gtk_box_pack_start(GTK_BOX(row), toggle, FALSE, FALSE, 0);
			std::string details = plugin.file.string();
			if (!plugin.description.empty())
				details = plugin.description + "\n" + details;
			if (!plugin.error.empty())
				details += "\nErro: " + plugin.error;
			else if (plugin.loaded)
				details += "\nCarregado (" + std::to_string(plugin.commands.size()) + " comandos no menu Plugins)";
			GtkWidget *info = gtk_label_new(details.c_str());
			gtk_label_set_xalign(GTK_LABEL(info), 0.0);
			gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
			gtk_widget_set_margin_start(info, 26);
			gtk_box_pack_start(GTK_BOX(row), info, FALSE, FALSE, 0);
			gtk_box_pack_start(GTK_BOX(list), row, FALSE, FALSE, 0);
		}
		gtk_widget_show_all(list);
	};
	fill();
	gtk_widget_show_all(dialog);
	for (;;)
	{
		const int response = gtk_dialog_run(GTK_DIALOG(dialog));
		if (response == 1)
			perform(Action::OpenPluginFolder);
		else if (response == 2)
		{
			scanPlugins(PluginManager::defaultDirectories());
			fill();
		}
		else
			break;
	}
	gtk_widget_destroy(dialog);
}

bool EditorWindow::runRecoverySmokeTest(const std::filesystem::path &directory, std::string &result)
{
	const auto marker = directory / "recovery-phase-1";
	std::error_code error;
	if (!std::filesystem::exists(marker))
	{
		if (!_documents.empty())
		{
			auto *initial = _documents.front().get();
			_sessionStore.discardBackup(*initial);
			_updating = true;
			gtk_notebook_remove_page(GTK_NOTEBOOK(_notebook), 0);
			_documents.erase(_documents.begin());
			_updating = false;
		}
		auto *document = createDocument();
		document->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("recuperação entre execuções"));
		document->displayName = "Rascunho persistente";
		// An existing file edited but not saved: closing keeps the edit, not the file.
		const auto existing = directory / "existente.txt";
		std::ofstream(existing, std::ios::binary | std::ios::trunc) << "original\n";
		openPath(existing);
		EditorDocument *edited = currentDocument();
		edited->send(SCI_DOCUMENTEND);
		edited->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("editado"));
		const bool keepsOnQuit = quitKeepsUnsaved() && backupDocuments();
		const bool stored = keepsOnQuit && _documents.size() == 2 && _sessionStore.backup(*_documents.front())
			&& std::filesystem::is_regular_file(_sessionStore.backupPath(*_documents.front()))
			&& std::filesystem::is_regular_file(_sessionStore.backupPath(*_documents.back()))
			&& _sessionStore.save(_documents);
		if (!stored)
		{
			result = "FAIL: backup inicial não foi persistido.\n";
			return false;
		}
		std::ofstream(marker) << "done\n";
		result = "PASS: backup inicial gravado.\n";
		return true;
	}
	std::ostringstream onDisk;
	onDisk << std::ifstream(directory / "existente.txt", std::ios::binary).rdbuf();
	const bool success = _documents.size() == 2 && _documents[0]->path.empty()
		&& _documents[0]->displayName == "Rascunho persistente"
		&& _documents[0]->text() == "recuperação entre execuções" && _documents[0]->send(SCI_GETMODIFY)
		&& _documents[1]->path.filename() == "existente.txt" && _documents[1]->text() == "original\neditado"
		&& _documents[1]->send(SCI_GETMODIFY) && onDisk.str() == "original\n";
	if (success)
	{
		// Undo goes back to the text that is on disk.
		_documents[1]->send(SCI_UNDO);
		if (_documents[1]->text() != "original\n" || _documents[1]->send(SCI_GETMODIFY))
		{
			result = "FAIL: desfazer depois da recuperação não voltou ao arquivo do disco.\n";
			return false;
		}
	}
	if (!success)
	{
		result = "FAIL: recuperação entre execuções: abas=" + std::to_string(_documents.size());
		for (const auto &document : _documents)
			result += " [" + document->displayName + ", texto=" + document->text() + ", caminho=" + document->path.string() + "]";
		result += "\n";
		return false;
	}
	result = "PASS: aba sem título e alterações não salvas de um arquivo restauradas em nova execução, marcadas como não salvas.\n";
	if (success)
	{
		for (const auto &document : _documents)
			_sessionStore.discardBackup(*document);
		std::filesystem::remove(marker, error);
	}
	return success;
}

bool EditorWindow::runSelfTest(const std::filesystem::path &directory, std::string &result)
{
	_selfTesting = true;
	_selfTestDirectory = directory;
	_settings.restoreSession = false;
	_sessionWritable = false;
	while (!_documents.empty())
		closeDocument(_documents.size() - 1, false, false);
	std::ostringstream log;
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	_sessionWritable = true;
	auto check = [&](bool condition, const char *message) {
		log << (condition ? "PASS: " : "FAIL: ") << message << '\n';
		return condition;
	};
	bool success = check(!error, "criar diretório temporário de testes");
	if (!success)
	{
		result = log.str();
		return false;
	}
	auto *document = createDocument("int main() {\n    // café na nuvem\n    return valor + valor + valor;\n}\n");
	check(document != nullptr, "criar uma aba com documento Scintilla");
	check(document->text().find("café na nuvem") != std::string::npos, "preservar texto Unicode");
	document->language = &languageForPath("teste.cpp");
	applyTheme(*document);
	check(document->send(SCI_GETLEXER) != 0, "ativar lexer C++ nativo do Lexilla");
	document->send(SCI_SETSEL, 0, 0);
	const SearchOptions options;
	const SearchResult found = find(*document, "valor", options);
	check(found.found, "localizar texto no documento");
	check(countMatches(*document, "valor", options) == 3, "contar todas as ocorrências");
	check(replaceAll(*document, "valor", "resultado", options) == 3, "substituir todas as ocorrências");
	check(document->text().find("resultado + resultado + resultado") != std::string::npos, "aplicar substituições ao texto");
	SearchOptions caseOptions;
	caseOptions.matchCase = true;
	check(countMatches(*document, "RESULTADO", caseOptions) == 0, "distinguir maiúsculas na busca");
	SearchOptions wordOptions;
	wordOptions.wholeWord = true;
	check(countMatches(*document, "resultad", wordOptions) == 0, "respeitar a opção palavra inteira");
	document->send(SCI_SETSEL, 0, 0);
	check(find(*document, "resultado", options, true).found, "localizar texto para trás");
	auto *regexDocument = createDocument("resultado resultado");
	SearchOptions regexOptions;
	regexOptions.regularExpression = true;
	check(countMatches(*regexDocument, "\\(resultado\\)", regexOptions) == 2, "localizar grupos regex do Scintilla");
	check(replaceAll(*regexDocument, "\\(resultado\\)", "total_\\1", regexOptions) == 2, "substituir grupos em documento isolado");
	check(regexDocument->text() == "total_resultado total_resultado", "expandir grupos na substituição regex");
	closeDocument(_documents.size() - 1, false);
	regexOptions.wrapAround = false;
	check(replaceAll(*document, "resultado", "valor_final", regexOptions) == 3, "substituir ocorrências usando o modo regex");
	check(goToLine(*document, 2), "navegar para uma linha válida");
	check(!goToLine(*document, 100), "rejeitar linha fora do documento");
	document->send(SCI_SETSEL, 0, 0);
	check(find(*document, "valor_final", options).found, "selecionar uma ocorrência encontrada");
	check(replaceCurrent(*document, "valor_final", "soma", options) == 1, "substituir a ocorrência selecionada");
	const std::string expected = document->text();
	const std::filesystem::path file = directory / "exemplo.cpp";
	std::string saveError;
	document->path = file;
	check(saveDocument(*document), "salvar documento nativamente");
	check(std::filesystem::is_regular_file(file), "confirmar arquivo salvo no disco");
	check(document->text() == expected && !document->send(SCI_GETMODIFY), "limpar estado de alterações após salvar");
	auto *loaded = createDocument();
	check(loaded->load(file, saveError), "abrir o arquivo salvo em outra aba");
	check(loaded->text() == expected, "manter conteúdo na leitura e escrita");
	check(loaded->language->id == "cpp", "detectar linguagem pela extensão");
	check(documentCount() == 2 && gtk_notebook_get_n_pages(GTK_NOTEBOOK(_notebook)) == 2, "gerenciar duas abas independentes");
	check(_documents[0].get() == document && _documents[1].get() == loaded, "manter ordem inicial dos documentos");
	show();
	setCurrentPage(0);
	check(currentDocument() == document, "alternar entre abas sem perder o documento");
	perform(Action::ConvertToWindowsEol);
	check(document->text().find("\r\n") != std::string::npos, "converter quebras de linha para Windows");
	perform(Action::ConvertToUnixEol);
	check(document->text().find("\r") == std::string::npos, "converter quebras de linha para Unix");
	const std::string beforeUndo = document->text();
	check(document->send(SCI_CANUNDO) != 0, "disponibilizar desfazer após edição");
	perform(Action::Undo);
	check(document->text() != beforeUndo, "desfazer altera o conteúdo do documento");
	check(document->send(SCI_CANREDO) != 0, "disponibilizar refazer após desfazer");
	perform(Action::Redo);
	check(document->text() == beforeUndo, "refazer restaura o conteúdo anterior");
	perform(Action::ToggleDarkTheme);
	check(_settings.darkTheme, "alternar tema escuro");
	perform(Action::ToggleDarkTheme);
	perform(Action::ToggleWordWrap);
	check(!_settings.wordWrap, "alternar quebra automática de linha");
	perform(Action::ToggleWordWrap);
	rememberRecentFile(_settings, file);
	check(!_settings.recentFiles.empty() && _settingsStore.save(_settings), "persistir configurações e arquivos recentes");
	const Settings restored = _settingsStore.load();
	check(restored.recentFiles.size() == 1 && restored.fontSize == _settings.fontSize, "recuperar configurações salvas");
	const auto latinFile = directory / "acentos.txt";
	{
		std::ofstream latin(latinFile, std::ios::binary);
		latin << "A\xe7\xe3o e cora\xe7\xe3o\r\n";
	}
	check(loaded->load(latinFile, saveError), "ler arquivo ISO-8859-1 sem interface externa");
	check(loaded->text().find("Ação e coração") != std::string::npos, "converter texto legado para UTF-8");
	check(loaded->eolMode == EolMode::Windows, "detectar quebras de linha CRLF");
	const std::filesystem::path oldContents = directory / "conteudo-antigo.txt";
	const std::filesystem::path emptyFile = directory / "vazio.txt";
	{
		std::ofstream output(oldContents);
		output << "conteúdo original";
		std::ofstream empty(emptyFile);
	}
	const std::filesystem::path readOnlyDirectory = directory / "somente-leitura";
	std::filesystem::create_directory(readOnlyDirectory);
	const std::filesystem::path readOnlyFile = readOnlyDirectory / "conteudo.txt";
	{
		std::ofstream output(readOnlyFile);
		output << "conteúdo original";
	}
	std::string ignoredError;
	std::filesystem::permissions(readOnlyDirectory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
	check(getuid() == 0 || (!loaded->save(readOnlyFile, ignoredError) && loaded->text().find("coração") != std::string::npos), "falha de gravação preserva o documento aberto");
	std::ifstream preserved(readOnlyFile, std::ios::binary);
	std::ostringstream preservedText;
	preservedText << preserved.rdbuf();
	check(preservedText.str() == "conteúdo original", "falha de gravação não apaga o arquivo existente");
	std::filesystem::permissions(readOnlyDirectory, std::filesystem::perms::owner_all);
	check(loaded->save(emptyFile, ignoredError), "salvar arquivo vazio no destino");
	check(closeDocument(currentIndex(), true) && documentCount() == 1, "fechar documento modificado e manter outra aba");

	auto *macroDocument = currentDocument();
	macroDocument->setText("");
	macroDocument->send(SCI_EMPTYUNDOBUFFER);
	perform(Action::StartMacro);
	macroDocument->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("macro á & <xml>"));
	macroDocument->send(SCI_GOTOPOS, 0);
	macroDocument->send(SCI_LINEEND);
	macroDocument->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("!"));
	perform(Action::StopMacro);
	check(_macro.size() >= 4 && !_recordingMacro, "gravar texto e comandos em uma macro");
	macroDocument->setText("");
	check(playMacro() && macroDocument->text() == "macro á & <xml>!", "reproduzir macro com texto Unicode");
	check(playMacro(2) && macroDocument->text() == "macro á & <xml>!macro á & <xml>!macro á & <xml>!", "repetir macro em uma única operação");
	macroDocument->send(SCI_UNDO);
	check(macroDocument->text() == "macro á & <xml>!", "desfazer repetição de macro em um passo");
	const SessionStore isolatedStore(directory / "nova-instancia");
	check(isolatedStore.saveMacro(_macro), "persistir a última macro");
	const auto reloadedMacro = isolatedStore.loadMacro();
	check(reloadedMacro.size() == _macro.size() && reloadedMacro.front().text == _macro.front().text, "recuperar macro persistida com caracteres escapados");
	check(!isolatedStore.saveMacro({{SCI_GETDIRECTPOINTER, 0, 0, false, ""}}), "rejeitar comandos arbitrários na macro persistida");
	check(isolatedStore.saveMacro({{SCI_REPLACESEL, 0, 0, true, ""}}) && isolatedStore.loadMacro().size() == 1
		&& isolatedStore.loadMacro()[0].hasText, "preservar comando textual vazio sem reinterpretá-lo como ponteiro");
	{
		std::ofstream invalid(directory / "nova-instancia" / "last-macro.xml");
		invalid << "<ScintillaMacro version=\"1\"><command message=\"0\" wparam=\"0\" lparam=\"0\" text=\"\"/></ScintillaMacro>";
	}
	check(isolatedStore.loadMacro().empty(), "descartar arquivo de macro inválido");
	perform(Action::ClearMacro);
	check(_macro.empty(), "limpar macro gravada");

	auto *untitled = createDocument("rascunho sem título");
	const auto untitledIndex = currentIndex();
	untitled->displayName = "Rascunho ágil";
	const std::string untitledId = untitled->recoveryId;
	const auto recoveryPath = _sessionStore.backupPath(*untitled);
	check(_sessionStore.backup(*untitled), "gravar backup de documento sem título");
	check(std::filesystem::is_regular_file(recoveryPath), "criar arquivo físico de recuperação");
	saveSession();
	check(_sessionStore.save(_documents), "persistir índice da sessão com recuperação");
	check(closeDocument(currentIndex(), false, false) && std::filesystem::is_regular_file(recoveryPath), "preservar backup ao sair sem descartar");
	const auto entries = _sessionStore.load();
	const auto recovered = std::find_if(entries.begin(), entries.end(), [&](const SessionEntry &entry) { return entry.backup == untitledId; });
	check(recovered != entries.end() && restoreEntry(*recovered), "restaurar documento sem título pelo índice da sessão");
	auto *recoveredDocument = currentDocument();
	check(recoveredDocument->text() == "rascunho sem título" && recoveredDocument->displayName == "Rascunho ágil", "preservar conteúdo e nome de aba recuperada");
	check(recoveredDocument->recoveryId == untitledId, "reutilizar identidade de recuperação sem duplicar backups");
	const auto savedUntitledFile = directory / "rascunho-recuperado.txt";
	recoveredDocument->path = savedUntitledFile;
	check(saveDocument(*recoveredDocument) && !std::filesystem::exists(recoveryPath), "salvar aba sem título recuperada e remover backup antigo");
	recoveredDocument->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("nova edição"));
	backupDocuments();
	check(!recoveredDocument->recoveryId.empty() && recoveredDocument->recoveryId != untitledId
		&& std::filesystem::is_regular_file(_sessionStore.backupPath(*recoveredDocument)), "proteger novas edições de arquivo recuperado sem reutilizar backup antigo");
	_sessionStore.discardBackup(*recoveredDocument);
	closeDocument(untitledIndex, false);
	check(!std::filesystem::exists(recoveryPath), "não preservar backup antigo depois de salvar a aba recuperada");
	check(_sessionStore.save(_documents), "gravar índice após salvar e fechar a aba recuperada");
	const auto savedAfterClose = _sessionStore.load();
	check(std::none_of(savedAfterClose.begin(), savedAfterClose.end(), [&](const SessionEntry &entry) { return entry.backup == untitledId; }), "remover identidade antiga do índice depois de salvar a recuperação");

	const auto recoveryFile = directory / "recuperado-salvo.txt";
	{
		std::ofstream output(recoveryFile);
		output << "conteúdo do disco";
	}
	auto *savedRecovery = createDocument();
	check(savedRecovery->load(recoveryFile, ignoredError), "abrir arquivo para teste de recuperação de alterações");
	savedRecovery->send(SCI_SETSEL, 0, 0);
	savedRecovery->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("edição recuperável "));
	check(savedRecovery->send(SCI_GETMODIFY) && _sessionStore.backup(*savedRecovery), "fazer backup de alterações em arquivo existente");
	SessionEntry modifiedEntry;
	modifiedEntry.name = savedRecovery->displayName;
	modifiedEntry.path = recoveryFile.string();
	modifiedEntry.backup = savedRecovery->recoveryId;
	modifiedEntry.modified = true;
	const auto modifiedBackup = _sessionStore.backupPath(*savedRecovery);
	closeDocument(currentIndex(), false, false);
	check(restoreEntry(modifiedEntry) && currentDocument()->text().find("edição recuperável") == 0, "recuperar alterações sem modificar o arquivo original");
	std::ifstream original(recoveryFile, std::ios::binary);
	std::ostringstream originalText;
	originalText << original.rdbuf();
	check(originalText.str() == "conteúdo do disco", "recuperação não altera automaticamente o arquivo no disco");
	check(saveDocument(*currentDocument()) && !std::filesystem::exists(modifiedBackup), "salvar recuperação e remover backup temporário");

	// Stage 3: find and replace in files, folders and open documents.
	const auto searchRoot = directory / "busca-em-pastas";
	std::filesystem::permissions(searchRoot / "protegido.h", std::filesystem::perms::owner_all, error);
	std::filesystem::remove_all(searchRoot, error);
	std::filesystem::remove(directory / "fora.h", error);
	error.clear();
	std::filesystem::create_directories(searchRoot / "sub", error);
	std::filesystem::create_directories(searchRoot / ".oculta", error);
	std::filesystem::create_directories(searchRoot / "build", error);
	auto writeFile = [](const std::filesystem::path &path, const std::string &bytes) {
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << bytes;
	};
	auto readFile = [](const std::filesystem::path &path) {
		std::ifstream input(path, std::ios::binary);
		std::ostringstream buffer;
		buffer << input.rdbuf();
		return buffer.str();
	};
	const std::string originalA = "alpha beta alpha\n\tint alpha_valor = 1;\n";
	writeFile(searchRoot / "a.cpp", originalA);
	writeFile(searchRoot / "sub" / "b.h", "Alpha\n");
	writeFile(searchRoot / ".oculta" / "c.cpp", "alpha\n");
	writeFile(searchRoot / "build" / "gerado.cpp", "alpha\n");
	writeFile(searchRoot / "notas.txt", "alpha\n");
	writeFile(searchRoot / "dados.bin", std::string("alpha\0alpha", 11));
	writeFile(searchRoot / "bom.h", "\xEF\xBB\xBF" "alpha\r\nfim\r\n");
	writeFile(searchRoot / "latin.txt", "alpha a\xe7\xe3o\n");
	check(!error, "criar árvore de pastas para busca em arquivos");
	auto resultFor = [](const FileSearchSummary &summary, const std::filesystem::path &path) -> const FileResult * {
		for (const auto &file : summary.files)
		{
			if (file.path == path)
				return &file;
		}
		return nullptr;
	};
	auto absolutePath = [](const std::filesystem::path &path) { return std::filesystem::absolute(path).lexically_normal(); };
	const auto fileA = absolutePath(searchRoot / "a.cpp");
	FileSearchOptions fileOptions;
	fileOptions.filters = "*.cpp *.h";
	const FileSearchSummary inFiles = findInFiles(searchRoot, "alpha", fileOptions);
	check(inFiles.error.empty() && inFiles.totalMatches() == 6 && inFiles.filesWithMatches() == 4, "localizar em pastas e subpastas com filtros");
	check(std::none_of(inFiles.files.begin(), inFiles.files.end(), [](const FileResult &file) { return file.path.string().find(".oculta") != std::string::npos; }), "ignorar pastas ocultas por padrão");
	const FileResult *resultA = resultFor(inFiles, fileA);
	check(resultA && resultA->matches.size() == 3 && resultA->matches[1].line == 1 && resultA->matches[1].column == 12
		&& resultA->matches[2].line == 2 && resultA->matches[2].column == 6, "informar linha e coluna de cada ocorrência");
	check(resultA && resultA->matches.size() == 3 && resultA->matches[2].lineText == "int alpha_valor = 1;", "mostrar o texto da linha encontrada");
	FileSearchOptions wordFiles = fileOptions;
	wordFiles.text.wholeWord = true;
	check(findInFiles(searchRoot, "alpha", wordFiles).totalMatches() == 5, "palavra inteira na busca em arquivos");
	FileSearchOptions caseFiles = fileOptions;
	caseFiles.text.matchCase = true;
	const auto caseSummary = findInFiles(searchRoot, "alpha", caseFiles);
	check(caseSummary.totalMatches() == 5 && !resultFor(caseSummary, absolutePath(searchRoot / "sub" / "b.h")), "diferenciar maiúsculas na busca em arquivos");
	FileSearchOptions flatFiles = fileOptions;
	flatFiles.recursive = false;
	const auto flatSummary = findInFiles(searchRoot, "alpha", flatFiles);
	check(flatSummary.totalMatches() == 4 && flatSummary.filesWithMatches() == 2, "pesquisar só a pasta escolhida sem subpastas");
	FileSearchOptions hiddenFiles = fileOptions;
	hiddenFiles.includeHidden = true;
	check(findInFiles(searchRoot, "alpha", hiddenFiles).totalMatches() == 7, "incluir pastas ocultas quando solicitado");
	FileSearchOptions excludedFiles;
	excludedFiles.filters = "* !*.txt !build/ !*.bin";
	const auto excludedSummary = findInFiles(searchRoot, "alpha", excludedFiles);
	check(excludedSummary.totalMatches() == 5 && excludedSummary.filesWithMatches() == 3, "excluir arquivos e pastas pelos filtros com !");
	FileSearchOptions everyFile;
	everyFile.filters = "*.*";
	const auto everySummary = findInFiles(searchRoot, "alpha", everyFile);
	check(everySummary.skippedBinary == 1 && everySummary.totalMatches() == 8, "ignorar binários e ler texto ISO-8859-1 na busca");
	check(FileFilter("*.CPP;!teste_*").acceptsFile("Main.cpp") && !FileFilter("*.CPP;!teste_*").acceptsFile("teste_a.cpp")
		&& !FileFilter("!+\\node_modules").acceptsDirectory("node_modules"), "interpretar filtros no estilo do Notepad++");
	FileSearchOptions regexFiles;
	regexFiles.filters = "*.cpp";
	regexFiles.text.regularExpression = true;
	check(findInFiles(searchRoot, "al\\(p\\)ha_", regexFiles).totalMatches() == 1, "expressão regular na busca em arquivos");
	check(!findInFiles(searchRoot / "nao-existe", "alpha", fileOptions).error.empty(), "avisar quando a pasta não existe");

	findInFiles(searchRoot, "alpha", fileOptions);
	check(_results.size() == 6 && gtk_tree_model_iter_n_children(GTK_TREE_MODEL(_resultsStore), nullptr) == 4
		&& gtk_widget_get_visible(_resultsPanel), "listar resultados agrupados por arquivo no painel");
	check(goToResult(1) && currentDocument() && currentDocument()->path == fileA
		&& currentDocument()->send(SCI_GETSELECTIONSTART) == 11 && currentDocument()->send(SCI_GETSELECTIONEND) == 16, "abrir arquivo e selecionar o resultado escolhido");
	check(nextResult() && _currentResult == 2 && currentDocument()->send(SCI_LINEFROMPOSITION, currentDocument()->send(SCI_GETCURRENTPOS)) == 1, "ir para o próximo resultado (F4)");
	check(nextResult(true) && _currentResult == 1, "voltar ao resultado anterior (Shift+F4)");
	GtkTreePath *firstRow = gtk_tree_path_new_from_string("0:0");
	activateResultRow(firstRow);
	gtk_tree_path_free(firstRow);
	check(_currentResult == 0 && currentDocument()->send(SCI_GETSELECTIONSTART) == 0, "abrir resultado pela linha do painel");
	auto *openA = currentDocument();
	openA->send(SCI_SETSEL, 0, 0);
	openA->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("alpha "));
	const auto unsavedSummary = findInFiles(searchRoot, "alpha", fileOptions);
	check(resultFor(unsavedSummary, fileA) && resultFor(unsavedSummary, fileA)->matches.size() == 4, "usar o conteúdo não salvo de arquivos abertos na busca");

	writeFile(directory / "fora.h", "alpha\n");
	std::filesystem::create_symlink(absolutePath(directory / "fora.h"), searchRoot / "atalho.h", error);
	writeFile(searchRoot / "protegido.h", "alpha\n");
	std::filesystem::permissions(searchRoot / "protegido.h", std::filesystem::perms::owner_read);
	FileSearchOptions replaceOptions;
	replaceOptions.filters = "*.cpp *.h *.txt";
	const auto replaced = replaceInFiles(searchRoot, "alpha", "gama", replaceOptions, false);
	check(openA->text().rfind("gama gama beta gama\n", 0) == 0 && readFile(fileA) == originalA, "substituir arquivo aberto só no editor, sem gravar no disco");
	check(readFile(searchRoot / "sub" / "b.h") == "gama\n" && readFile(searchRoot / "build" / "gerado.cpp") == "gama\n"
		&& readFile(searchRoot / "notas.txt") == "gama\n", "substituir em arquivos fechados no disco");
	check(readFile(searchRoot / "bom.h") == "\xEF\xBB\xBF" "gama\r\nfim\r\n", "preservar BOM e CRLF ao substituir");
	const FileResult *latinResult = resultFor(replaced, absolutePath(searchRoot / "latin.txt"));
	check(readFile(searchRoot / "latin.txt") == "alpha a\xe7\xe3o\n" && latinResult && !latinResult->error.empty(), "não converter arquivos com codificação diferente de UTF-8");
	check(!error && std::filesystem::is_symlink(searchRoot / "atalho.h") && readFile(directory / "fora.h") == "gama\n", "manter atalhos simbólicos e alterar o arquivo de destino");
	const FileResult *protectedResult = resultFor(replaced, absolutePath(searchRoot / "protegido.h"));
	check(getuid() == 0 || (readFile(searchRoot / "protegido.h") == "alpha\n" && protectedResult && !protectedResult->error.empty()), "não sobrescrever arquivos somente leitura");
	check(readFile(searchRoot / ".oculta" / "c.cpp") == "alpha\n", "não substituir em pastas ocultas por padrão");
	check(replaced.totalReplaced() == (getuid() == 0 ? 10u : 9u), "contar substituições em arquivos");
	bool temporaryLeft = false;
	for (const auto &entry : std::filesystem::recursive_directory_iterator(searchRoot))
		temporaryLeft = temporaryLeft || entry.path().filename().string().find(".npp-linux-") != std::string::npos;
	check(!temporaryLeft, "não deixar arquivos temporários após substituir");
	openA->send(SCI_UNDO);
	check(openA->text().rfind("alpha alpha beta alpha\n", 0) == 0, "desfazer substituição feita em arquivo aberto");
	std::filesystem::permissions(searchRoot / "protegido.h", std::filesystem::perms::owner_all);

	auto *scratchDocument = createDocument("um dois um");
	const auto openSummary = findInOpenDocuments("um", SearchOptions());
	const auto scratchResult = std::find_if(openSummary.files.begin(), openSummary.files.end(), [&](const FileResult &file) { return file.document == scratchDocument; });
	check(scratchResult != openSummary.files.end() && scratchResult->matches.size() == 2 && scratchResult->path.empty(), "localizar em todos os documentos abertos, inclusive sem título");
	setCurrentPage(0);
	findInOpenDocuments("dois", SearchOptions());
	check(goToResult(0) && currentDocument() == scratchDocument && scratchDocument->send(SCI_GETSELECTIONSTART) == 3, "navegar até resultado em aba sem título");
	check(replaceInOpenDocuments("um", "três", SearchOptions()).totalReplaced() >= 2 && scratchDocument->text() == "três dois três", "substituir em todos os documentos abertos");

	_settings.findFilters = "*.cpp !build/";
	_settings.findFolder = searchRoot.string();
	_settings.findRecursive = false;
	check(_settingsStore.save(_settings), "salvar preferências da busca em arquivos");
	const Settings findSettings = _settingsStore.load();
	check(findSettings.findFilters == "*.cpp !build/" && findSettings.findFolder == searchRoot.string() && !findSettings.findRecursive, "recuperar pasta e filtros da última busca");
	perform(Action::FindInFiles);
	check(_findDialog && gtk_widget_get_visible(_findDialog) && std::string(gtk_entry_get_text(GTK_ENTRY(_findFolder))) == searchRoot.string(), "abrir a janela Localizar em arquivos com a última pasta");
	gtk_entry_set_text(GTK_ENTRY(_findText), "gama");
	gtk_entry_set_text(GTK_ENTRY(_findFilters), "*.h");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_findRecursive), TRUE);
	runFindInFilesDialog(FindAllInFiles);
	check(_results.size() == 3, "executar busca pela janela Localizar em arquivos");
	runFindInFilesDialog(GTK_RESPONSE_CLOSE);
	check(!gtk_widget_get_visible(_findDialog), "fechar a janela Localizar em arquivos");
	perform(Action::ToggleResults);
	check(!gtk_widget_get_visible(_resultsPanel), "ocultar o painel de resultados");
	closeDocument(currentIndex(), false);

	// Stage 4: function list, document map and workspace.
	auto names = [](const std::vector<FunctionEntry> &entries) {
		std::vector<std::string> result;
		for (const auto &entry : entries)
			result.push_back(entry.name);
		return result;
	};
	auto entryNamed = [](const std::vector<FunctionEntry> &entries, const std::string &name) -> const FunctionEntry * {
		for (const auto &entry : entries)
		{
			if (entry.name == name)
				return &entry;
		}
		return nullptr;
	};
	const std::string cppSample =
		"#include <vector>\n"
		"// void comentario() {}\n"
		"namespace app {\n"
		"class Janela : public Base {\n"
		"public:\n"
		"    void abrir(int x) {\n"
		"        if (x) { return; }\n"
		"        auto f = [](int y) { return y; };\n"
		"    }\n"
		"    int contar() const;\n"
		"};\n"
		"int Janela::contar() const {\n"
		"    const char *texto = \"void falso() {\";\n"
		"    for (int i = 0; i < 3; ++i) {}\n"
		"    return 1;\n"
		"}\n"
		"}\n"
		"struct Ponto { int x; };\n"
		"static int soma(int a, int b)\n"
		"{\n"
		"    return a + b;\n"
		"}\n";
	const auto cppFunctions = parseFunctions("cpp", cppSample);
	check(names(cppFunctions) == std::vector<std::string>{"app", "Janela", "abrir", "Janela::contar", "Ponto", "soma"},
		"listar namespace, classes, métodos e funções C++ ignorando comentários, textos e blocos de controle");
	const FunctionEntry *abrir = entryNamed(cppFunctions, "abrir");
	const FunctionEntry *soma = entryNamed(cppFunctions, "soma");
	check(abrir && abrir->line == 6 && abrir->parent == "Janela" && abrir->kind == "método"
		&& soma && soma->line == 19 && cppSample.compare(soma->position, 4, "soma") == 0 && soma->kind == "função",
		"informar linha, posição, tipo e classe de cada função");
	const auto pythonFunctions = parseFunctions("python",
		"class Modelo:\n"
		"    \"\"\"def falso(): pass\"\"\"\n"
		"    def salvar(self):\n"
		"        def interno():\n"
		"            pass\n"
		"        return 1\n"
		"\n"
		"async def principal():\n"
		"    pass\n");
	check(names(pythonFunctions) == std::vector<std::string>{"Modelo", "salvar", "principal"}
		&& pythonFunctions[1].parent == "Modelo", "listar classes e funções Python pela indentação");
	const std::string jsSample =
		"// function comentada() {}\n"
		"function iniciar() { const s = `function x() {`; }\n"
		"const somar = (a, b) => { return a + b; };\n"
		"class Carrinho extends Base {\n"
		"  adicionar(item) { if (item) { } }\n"
		"  get total() { return 0; }\n"
		"}\n"
		"describe('x', () => { it('y', function () {}); });\n";
	check(names(parseFunctions("javascript", jsSample)) == std::vector<std::string>{"iniciar", "somar", "Carrinho", "adicionar", "total"},
		"listar funções, funções de seta, classes e métodos JavaScript");
	check(names(parseFunctions("go",
		"package main\n"
		"type Servidor struct {\n\tporta int\n}\n"
		"func (s *Servidor) Iniciar() error {\n\tif err := s.abrir(); err != nil {\n\t\treturn err\n\t}\n\treturn nil\n}\n"
		"func main() {}\n")) == std::vector<std::string>{"Servidor", "Iniciar", "main"}, "listar tipos e funções Go");
	check(names(parseFunctions("rust",
		"#[derive(Debug)]\nstruct Ponto { x: i32 }\nimpl Ponto {\n    pub fn novo() -> Self { Ponto { x: 0 } }\n}\nfn main() { let c = 'a'; }\n"))
		== std::vector<std::string>{"Ponto", "Ponto", "novo", "main"}, "listar structs, impl e funções Rust");
	const auto headings = parseFunctions("markdown", "# Título\n```\n# não é título\n```\n## Seção dois\n");
	check(names(headings) == std::vector<std::string>{"Título", "Seção dois"} && headings[1].kind == "H2", "listar títulos Markdown fora de blocos de código");
	check(names(parseFunctions("shell", "iniciar() {\n  :\n}\nfunction parar {\n  :\n}\n# falso() {\n")) == std::vector<std::string>{"iniciar", "parar"},
		"listar funções de shell script");
	check(!functionListSupports("json") && functionListSupports("python") && functionListSupports("sql"), "informar linguagens sem lista de funções");

	auto *outline = createDocument(cppSample);
	outline->language = &languageForPath("lista.cpp");
	applyTheme(*outline);
	perform(Action::ToggleFunctionList);
	check(_settings.showFunctionList && gtk_widget_get_visible(_sidebar) && gtk_widget_get_visible(_functionPanel), "mostrar o painel Lista de funções");
	check(_functions.size() == 6 && gtk_tree_model_iter_n_children(GTK_TREE_MODEL(_functionStore), nullptr) == 3, "agrupar a lista de funções por classe e namespace");
	GtkTreePath *somaRow = gtk_tree_path_new_from_string("2");
	activateFunctionRow(somaRow);
	gtk_tree_path_free(somaRow);
	check(outline->send(SCI_GETSELECTIONSTART) == static_cast<sptr_t>(soma->position)
		&& outline->send(SCI_GETSELECTIONEND) == static_cast<sptr_t>(soma->position + 4), "ir até a função escolhida na lista");
	gtk_entry_set_text(GTK_ENTRY(_functionFilter), "ABR");
	check(_functions.size() == 6 && gtk_tree_model_iter_n_children(GTK_TREE_MODEL(_functionStore), nullptr) == 1, "filtrar a lista de funções sem diferenciar maiúsculas");
	gtk_entry_set_text(GTK_ENTRY(_functionFilter), "");
	outline->setText(jsSample);
	outline->language = &languageForPath("lista.js");
	applyTheme(*outline);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_functionSort), TRUE);
	GtkTreeIter firstFunction;
	gchar *firstName = nullptr;
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(_functionStore), &firstFunction))
		gtk_tree_model_get(GTK_TREE_MODEL(_functionStore), &firstFunction, FunctionText, &firstName, -1);
	check(firstName && std::string(firstName) == "Carrinho", "ordenar a lista de funções por nome");
	g_free(firstName);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_functionSort), FALSE);
	outline->send(SCI_DOCUMENTEND);
	outline->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("function nova() {}\n"));
	scheduleFunctionList(true);
	check(entryNamed(_functions, "nova") != nullptr, "atualizar a lista de funções após editar");

	std::string longText;
	for (int line = 1; line <= 400; ++line)
		longText += "linha " + std::to_string(line) + "\n";
	outline->setText(longText);
	perform(Action::ToggleDocumentMap);
	check(_settings.showDocumentMap && gtk_widget_get_visible(_mapPanel) && gtk_notebook_get_show_tabs(GTK_NOTEBOOK(_sidebar)), "mostrar o mapa do documento junto da lista de funções");
	check(_mapView->send(SCI_GETDOCPOINTER) == outline->send(SCI_GETDOCPOINTER) && _mapView->send(SCI_GETZOOM) == -10, "mapa reutiliza o documento atual em miniatura");
	// Let GTK finish layout and scroll ranges, as the running application does between clicks.
	while (gtk_events_pending())
		gtk_main_iteration();
	outline->send(SCI_SETSEL, 5, 5);
	outline->send(SCI_SETFIRSTVISIBLELINE, 100);
	syncDocumentMap();
	check(_mapView->send(SCI_GETSELECTIONSTART) == outline->send(SCI_POSITIONFROMLINE, 100), "destacar no mapa a área visível do editor");
	check(outline->send(SCI_GETSELECTIONSTART) == 5 && outline->send(SCI_GETSELECTIONEND) == 5, "mapa não altera a seleção do editor");
	while (gtk_events_pending())
		gtk_main_iteration();
	documentMapJumpToLine(300);
	const sptr_t firstAfterJump = outline->send(SCI_GETFIRSTVISIBLELINE);
	const sptr_t linesOnScreen = std::max<sptr_t>(outline->send(SCI_LINESONSCREEN), 1);
	check(firstAfterJump <= 300 && firstAfterJump >= 300 - linesOnScreen, "navegar pelo mapa até outra parte do documento");
	const std::size_t lengthBeforeMap = outline->text().size();
	documentMapScroll(3);
	check(outline->text().size() == lengthBeforeMap && outline->send(SCI_GETFIRSTVISIBLELINE) == std::min<sptr_t>(firstAfterJump + 3, outline->send(SCI_GETLINECOUNT)), "rolar o editor pelo mapa sem editar o texto");
	auto *secondMapped = createDocument("outra aba\n");
	syncDocumentMap();
	check(_mapView->send(SCI_GETDOCPOINTER) == secondMapped->send(SCI_GETDOCPOINTER), "mapa acompanha a troca de abas");
	closeDocument(currentIndex(), false);
	syncDocumentMap();
	check(_mapView->send(SCI_GETDOCPOINTER) == currentDocument()->send(SCI_GETDOCPOINTER), "mapa volta ao documento atual ao fechar uma aba");
	perform(Action::ToggleFunctionList);
	check(!gtk_widget_get_visible(_functionPanel) && gtk_widget_get_visible(_sidebar) && !gtk_notebook_get_show_tabs(GTK_NOTEBOOK(_sidebar)), "ocultar a lista mantendo o mapa");
	perform(Action::ToggleDocumentMap);
	check(!gtk_widget_get_visible(_sidebar), "ocultar a barra lateral quando os dois painéis estão fechados");

	const auto workspaceRoot = directory / "espaço & <projeto>";
	std::filesystem::remove_all(workspaceRoot, error);
	error.clear();
	std::filesystem::create_directories(workspaceRoot / "src", error);
	std::filesystem::create_directories(workspaceRoot / "docs", error);
	std::filesystem::create_directories(workspaceRoot / ".git", error);
	writeFile(workspaceRoot / "src" / "main.cpp", "int main() { return 0; }\n");
	writeFile(workspaceRoot / "LEIAME.md", "# Projeto\n");
	writeFile(workspaceRoot / "a.txt", "a\n");
	std::string listError;
	const auto listing = listDirectory(workspaceRoot, false, listError);
	std::vector<std::string> listed;
	for (const auto &entry : listing)
		listed.push_back(entry.name);
	check(listError.empty() && listed == std::vector<std::string>{"docs", "src", "a.txt", "LEIAME.md"}, "listar pastas antes de arquivos e ocultar itens escondidos");
	check(addWorkspaceFolder(workspaceRoot) && _settings.workspaceFolders.size() == 1 && gtk_widget_get_visible(_workspacePanel)
		&& gtk_tree_model_iter_n_children(GTK_TREE_MODEL(_workspaceStore), nullptr) == 1, "adicionar pasta ao espaço de trabalho");
	check(addWorkspaceFolder(workspaceRoot) && _settings.workspaceFolders.size() == 1, "não duplicar pastas do espaço de trabalho");
	GtkTreePath *rootRow = gtk_tree_path_new_from_string("0");
	gtk_tree_view_expand_row(GTK_TREE_VIEW(_workspaceView), rootRow, FALSE);
	GtkTreeIter rootIter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(_workspaceStore), &rootIter, rootRow);
	check(gtk_tree_model_iter_n_children(GTK_TREE_MODEL(_workspaceStore), &rootIter) == 4, "carregar o conteúdo da pasta ao expandir");
	gtk_tree_path_free(rootRow);
	GtkTreePath *srcRow = gtk_tree_path_new_from_string("0:1");
	activateWorkspaceRow(srcRow);
	gtk_tree_path_free(srcRow);
	GtkTreePath *mainRow = gtk_tree_path_new_from_string("0:1:0");
	activateWorkspaceRow(mainRow);
	check(currentDocument() && currentDocument()->path == absolutePath(workspaceRoot / "src" / "main.cpp"), "abrir arquivo pelo espaço de trabalho");
	gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(_workspaceView)), mainRow);
	gtk_tree_path_free(mainRow);
	findInSelectedWorkspaceFolder();
	check(std::string(gtk_entry_get_text(GTK_ENTRY(_findFolder))) == (absolutePath(workspaceRoot / "src")).string(), "localizar em arquivos a partir da pasta selecionada");
	runFindInFilesDialog(GTK_RESPONSE_CLOSE);
	const auto workspaceFile = directory / "projeto.npp-workspace";
	check(saveWorkspaceFile(workspaceFile), "salvar arquivo de espaço de trabalho");
	std::string workspaceError;
	const auto savedFolders = loadWorkspaceFile(workspaceFile, workspaceError);
	check(workspaceError.empty() && savedFolders == _settings.workspaceFolders, "ler espaço de trabalho com caracteres especiais");
	GtkTreePath *selectedChild = gtk_tree_path_new_from_string("0:1:0");
	gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(_workspaceView)), selectedChild);
	gtk_tree_path_free(selectedChild);
	removeSelectedWorkspaceFolder();
	check(_settings.workspaceFolders.empty() && std::filesystem::exists(workspaceRoot / "src" / "main.cpp"), "remover pasta do espaço de trabalho sem apagar arquivos");
	check(openWorkspaceFile(workspaceFile) && _settings.workspaceFolders == savedFolders, "abrir arquivo de espaço de trabalho");
	writeFile(directory / "invalido.npp-workspace", "<qualquer/>");
	check(!openWorkspaceFile(directory / "invalido.npp-workspace") && _settings.workspaceFolders == savedFolders, "rejeitar arquivo que não é espaço de trabalho");
	_settings.showFunctionList = true;
	_settings.functionListSorted = true;
	check(_settingsStore.save(_settings), "salvar preferências dos painéis");
	const Settings panelSettings = _settingsStore.load();
	check(panelSettings.showWorkspace && panelSettings.showFunctionList && !panelSettings.showDocumentMap && panelSettings.functionListSorted
		&& panelSettings.workspaceFolders == savedFolders, "recuperar painéis abertos e pastas do espaço de trabalho");
	_settings.showFunctionList = false;
	perform(Action::ToggleWorkspace);
	check(!gtk_widget_get_visible(_workspacePanel), "ocultar o espaço de trabalho");
	while (documentCount() > 1)
		closeDocument(documentCount() - 1, false);

	// Stage 5: styles, preferences, user languages, line tools and plugins.
	const Theme customTheme = parseTheme("comment=#FF0000/-/1/0;bogus=1;keyword=zz", false);
	check(customTheme[ThemeItem::Comment].fore == rgb(255, 0, 0) && customTheme[ThemeItem::Comment].bold && !customTheme[ThemeItem::Comment].italic
		&& customTheme[ThemeItem::Keyword].fore == defaultTheme(false)[ThemeItem::Keyword].fore, "ler cores personalizadas ignorando valores inválidos");
	check(serialiseTheme(parseTheme(serialiseTheme(customTheme), false)) == serialiseTheme(customTheme), "gravar e reler o esquema de cores");
	auto *styled = createDocument("// comentário\nint x;\n");
	styled->language = &languageForPath("cores.cpp");
	_settings.lightStyles = "comment=#FF0000/-/1/0;text=#101010/#FAFAF0/0/0";
	// The style configurator's categories are used when the Windows colours are off.
	_settings.nppLanguageColors = false;
	applySettings();
	const int commentStyle = styled->language->commentStyles.front();
	check(styled->send(SCI_STYLEGETFORE, static_cast<uptr_t>(commentStyle)) == rgb(255, 0, 0) && styled->send(SCI_STYLEGETBOLD, static_cast<uptr_t>(commentStyle))
		&& styled->send(SCI_STYLEGETBACK, STYLE_DEFAULT) == rgb(250, 250, 240), "aplicar o configurador de estilos ao editor");
	{
		// Colours per language as in the Windows version.
		_settings.nppLanguageColors = true;
		_settings.darkTheme = false;
		applySettings();
		const std::string code = "#include <x>\nint main() { return 42; } // fim\nstd::string s = \"txt\";\n";
		styled->setText(code);
		styled->send(SCI_COLOURISE, 0, -1);
		auto styleOf = [&](const std::string &word) { return static_cast<int>(styled->send(SCI_GETSTYLEAT, code.find(word))); };
		auto foreOf = [&](int style) { return static_cast<int>(styled->send(SCI_STYLEGETFORE, static_cast<uptr_t>(style))); };
		check(styleOf("return") == SCE_C_WORD && foreOf(SCE_C_WORD) == rgb(0, 0, 255) && styled->send(SCI_STYLEGETBOLD, SCE_C_WORD)
			&& foreOf(styleOf("42")) == rgb(255, 128, 0) && foreOf(styleOf("// fim")) == rgb(0, 128, 0)
			&& foreOf(styleOf("#include")) == rgb(128, 64, 0) && foreOf(styleOf("\"txt")) == rgb(128, 128, 128)
			&& foreOf(SCE_C_WORD2) == rgb(128, 0, 255) && foreOf(SCE_C_OPERATOR) == rgb(0, 0, 128),
			"cores do C++ iguais às da versão Windows (palavras, números, comentários, textos, pré-processador)");
		styled->language = &languageForPath("x.py");
		styled->path = "x.py";
		applyTheme(*styled);
		const std::string python = "def soma(a):\n    return a + 1  # fim\n";
		styled->setText(python);
		styled->send(SCI_COLOURISE, 0, -1);
		check(styled->send(SCI_GETSTYLEAT, 0) == SCE_P_WORD && foreOf(SCE_P_WORD) == rgb(0, 0, 255)
			&& foreOf(static_cast<int>(styled->send(SCI_GETSTYLEAT, 4))) == rgb(255, 0, 255), "cores do Python iguais às da versão Windows (nome da função em destaque)");
		_settings.darkTheme = true;
		applySettings();
		const int darkKeyword = foreOf(SCE_P_WORD);
		_settings.darkTheme = false;
		applySettings();
		check(darkKeyword != rgb(0, 0, 255) && darkKeyword >= 0, "tema escuro usa as cores do tema escuro do Windows");
		styled->language = &languageForPath("x.cpp");
		styled->path.clear();
	}
	_settings.lightStyles.clear();
	_settings.edgeColumn = 80;
	_settings.indentGuides = true;
	_settings.highlightCaretLine = false;
	applySettings();
	check(styled->send(SCI_GETEDGECOLUMN) == 80 && styled->send(SCI_GETEDGEMODE) == EDGE_LINE
		&& styled->send(SCI_GETINDENTATIONGUIDES) == SC_IV_LOOKBOTH && styled->send(SCI_GETCARETLINEVISIBLE) == 0
		&& styled->send(SCI_STYLEGETBACK, STYLE_DEFAULT) == defaultTheme(false)[ThemeItem::Text].back, "aplicar linha-guia, guias de recuo e destaque da linha atual");
	_settings.edgeColumn = 0;
	_settings.indentGuides = false;
	_settings.highlightCaretLine = true;
	applySettings();

	styled->setText("    abc");
	styled->send(SCI_DOCUMENTEND);
	styled->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("\n"));
	charAdded(styled, '\n');
	check(styled->send(SCI_GETLINEINDENTATION, 1) == 4 && styled->send(SCI_GETCURRENTPOS) == styled->send(SCI_GETLINEINDENTPOSITION, 1), "manter o recuo ao pressionar Enter");
	_settings.autoCloseBrackets = true;
	styled->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("f("));
	charAdded(styled, '(');
	check(styled->text() == "    abc\n    f()" && styled->send(SCI_GETCURRENTPOS) == static_cast<sptr_t>(styled->text().size()) - 1, "fechar parênteses automaticamente");
	_settings.autoCloseBrackets = false;
	updateBraceMatch(*styled);

	_settings.defaultEolWindows = true;
	_settings.defaultLanguage = "python";
	perform(Action::NewFile);
	check(currentDocument()->send(SCI_GETEOLMODE) == SC_EOL_CRLF && currentDocument()->language->id == "python", "aplicar fim de linha e linguagem padrão a novos documentos");
	check(commentPrefix(*currentDocument()) == "#", "escolher o comentário de linha pela linguagem");
	closeDocument(currentIndex(), false);
	_settings.maxRecentFiles = 2;
	Settings recentSettings = _settings;
	recentSettings.recentFiles.clear();
	rememberRecentFile(recentSettings, directory / "um.txt");
	rememberRecentFile(recentSettings, directory / "dois.txt");
	rememberRecentFile(recentSettings, directory / "tres.txt");
	check(recentSettings.recentFiles.size() == 2, "limitar a quantidade de arquivos recentes");
	_settings.backupInterval = 45;
	_settings.enabledPlugins = {"text-tools.so"};
	_settings.lightStyles = serialiseTheme(customTheme);
	check(_settingsStore.save(_settings), "salvar preferências ampliadas");
	const Settings extended = _settingsStore.load();
	check(extended.defaultEolWindows && extended.defaultLanguage == "python" && extended.maxRecentFiles == 2 && extended.backupInterval == 45
		&& extended.enabledPlugins == std::vector<std::string>{"text-tools.so"} && extended.lightStyles == _settings.lightStyles && !extended.autoCloseBrackets,
		"recuperar preferências, cores e plugins ativados");
	_settings = Settings();
	_settings.restoreSession = false;
	applySettings();

	styled->setText("b\na\nc\na\n");
	styled->send(SCI_SETSEL, 0, 0);
	check(sortLines(*styled, false, true) && styled->text() == "a\na\nb\nc\n", "ordenar linhas de A a Z");
	styled->send(SCI_SETSEL, 0, 0);
	check(removeDuplicateLines(*styled) == 1 && styled->text() == "a\nb\nc\n", "remover linhas duplicadas");
	styled->send(SCI_SETSEL, 0, 0);
	check(sortLines(*styled, true, true) && styled->text() == "c\nb\na\n", "ordenar linhas de Z a A");
	styled->send(SCI_UNDO);
	check(styled->text() == "a\nb\nc\n", "desfazer ordenação em um passo");
	styled->setText("x  \ny\t\n");
	check(trimTrailingWhitespace(*styled) == 2 && styled->text() == "x\ny\n", "remover espaços no fim das linhas");
	styled->setText("int a;\n  int b;\n");
	styled->send(SCI_SETSEL, 0, styled->send(SCI_GETLENGTH));
	check(toggleLineComment(*styled, "//") && styled->text() == "// int a;\n  // int b;\n", "comentar linhas selecionadas");
	styled->send(SCI_SETSEL, 0, styled->send(SCI_GETLENGTH));
	check(toggleLineComment(*styled, "//") && styled->text() == "int a;\n  int b;\n", "descomentar linhas selecionadas");
	styled->setText("linha");
	styled->send(SCI_SETSEL, 0, 0);
	perform(Action::DuplicateLine);
	check(styled->text() == "linha\nlinha", "duplicar linha");
	styled->setText("abc");
	styled->send(SCI_SETSEL, 0, 3);
	perform(Action::UpperCase);
	check(styled->text() == "ABC", "converter seleção para maiúsculas");

	const std::string udlXml =
		"<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<NotepadPlus>\n"
		"    <UserLang name=\"Minha Linguagem\" ext=\"minha mlg\" udlVersion=\"2.1\">\n"
		"        <Settings><Global caseIgnored=\"yes\" /></Settings>\n"
		"        <KeywordLists>\n"
		"            <Keywords name=\"Comments\">00// 01 02 03/* 04*/</Keywords>\n"
		"            <Keywords name=\"Operators1\">+ - = ==</Keywords>\n"
		"            <Keywords name=\"Keywords1\">se senao fim</Keywords>\n"
		"            <Keywords name=\"Keywords2\">verdadeiro falso</Keywords>\n"
		"            <Keywords name=\"Delimiters\">00&quot; 01\\ 02&quot; 03&apos; 04 05&apos;</Keywords>\n"
		"        </KeywordLists>\n"
		"        <Styles>\n"
		"            <WordsStyle name=\"KEYWORDS1\" fgColor=\"FF0000\" bgColor=\"FFFFFF\" fontStyle=\"1\" />\n"
		"        </Styles>\n"
		"    </UserLang>\n</NotepadPlus>\n";
	std::vector<UserLanguage> parsedLanguages;
	std::string udlError;
	check(parseUserLanguages(udlXml, parsedLanguages, udlError) && parsedLanguages.size() == 1, "ler linguagem no formato XML do Notepad++");
	if (parsedLanguages.size() == 1)
	{
		const UserLanguage &parsed = parsedLanguages.front();
		check(parsed.name == "Minha Linguagem" && parsed.extensions == std::vector<std::string>{"minha", "mlg"} && parsed.caseIgnored
			&& parsed.lineComments == std::vector<std::string>{"//"} && parsed.blockOpen == "/*" && parsed.blockClose == "*/"
			&& parsed.delimiters[0].open == "\"" && parsed.delimiters[0].escape == "\\" && parsed.delimiters[1].open == "'"
			&& parsed.keywords[1] == std::vector<std::string>{"verdadeiro", "falso"} && parsed.styles.count(UserKeyword1)
			&& parsed.styles.at(UserKeyword1).fore == rgb(255, 0, 0) && parsed.styles.at(UserKeyword1).bold, "interpretar comentários, palavras, delimitadores e cores da linguagem");
		std::vector<UserLanguage> roundTrip;
		std::string roundTripError;
		check(parseUserLanguages(userLanguageToXml(parsed), roundTrip, roundTripError) && roundTrip.size() == 1
			&& roundTrip[0].keywords == parsed.keywords && roundTrip[0].operators == parsed.operators
			&& roundTrip[0].delimiters[0].escape == "\\" && roundTrip[0].lineComments == parsed.lineComments
			&& roundTrip[0].styles.at(UserKeyword1).fore == rgb(255, 0, 0), "exportar linguagem em XML compatível e reler");
	}
	check(!parseUserLanguages("<NotepadPlus/>", parsedLanguages, udlError) && !udlError.empty(), "rejeitar XML sem linguagem");
	writeFile(directory / "minha-linguagem.xml", udlXml);
	std::string importError;
	check(importUserLanguage(directory / "minha-linguagem.xml", importError) == 1 && _userLanguages.size() == 1
		&& std::filesystem::is_regular_file(userLanguageDirectory() / "Minha_Linguagem.xml"), "importar linguagem do usuário");
	check(importUserLanguage(directory / "minha-linguagem.xml", importError) == 1 && _userLanguages.size() == 1, "reimportar sem duplicar a linguagem");
	std::size_t languageItems = 0;
	GList *languageChildren = gtk_container_get_children(GTK_CONTAINER(_languageMenu));
	for (GList *entry = languageChildren; entry; entry = entry->next)
		languageItems += g_object_get_data(G_OBJECT(entry->data), "npp-language") ? 1 : 0;
	g_list_free(languageChildren);
	check(languageItems == languages().size() + 1, "mostrar a linguagem do usuário no menu Linguagem");
	const std::string udlText = "SE x == 10 // nota\n/* bloco\ncontinua */ \"te\\\"xto\" verdadeiro\n";
	writeFile(directory / "exemplo.minha", udlText);
	openPath(directory / "exemplo.minha");
	EditorDocument *udlDocument = currentDocument();
	check(udlDocument && udlDocument->language->id == "udl:Minha Linguagem", "reconhecer a linguagem do usuário pela extensão");
	auto styleAt = [&](std::size_t position) { return static_cast<int>(udlDocument->send(SCI_GETSTYLEAT, position)); };
	check(styleAt(0) == UserKeyword1 && styleAt(udlText.find("==")) == UserOperator && styleAt(udlText.find("10")) == UserNumber
		&& styleAt(udlText.find("x ")) == UserDefault && styleAt(udlText.find("// nota") + 3) == UserLineComment, "realçar palavras-chave, operadores, números e comentários");
	check(styleAt(udlText.find("continua")) == UserComment && styleAt(udlText.find("\"te") + 4) == UserDelimiter1
		&& styleAt(udlText.find("xto")) == UserDelimiter1 && styleAt(udlText.find("verdadeiro")) == UserKeyword1 + 1, "realçar comentários de várias linhas e textos com escape");
	check(udlDocument->send(SCI_STYLEGETFORE, UserKeyword1) == rgb(255, 0, 0) && udlDocument->send(SCI_STYLEGETBOLD, UserKeyword1), "usar as cores definidas na linguagem do usuário");
	check(commentPrefix(*udlDocument) == "//", "comentar linhas com o prefixo da linguagem do usuário");
	UserLanguage changed = _userLanguages.front();
	changed.keywords[0].push_back("enquanto");
	std::string changeError;
	check(saveUserLanguageDefinition(changed, changeError), "salvar alterações na linguagem do usuário");
	udlDocument->setText("enquanto");
	udlDocument->send(SCI_COLOURISE, 0, -1);
	check(styleAt(0) == UserKeyword1 && udlDocument->language->id == "udl:Minha Linguagem", "atualizar o realce depois de editar a linguagem");
	UserLanguage clash;
	clash.name = "Python";
	check(!saveUserLanguageDefinition(clash, changeError), "não substituir linguagens embutidas");
	check(removeUserLanguage("Minha Linguagem") && _userLanguages.empty()
		&& std::filesystem::is_regular_file(userLanguageDirectory() / "removidas" / "Minha_Linguagem.xml")
		&& udlDocument->language->id == "text" && languageFor("outro.minha").id != "udl:Minha Linguagem", "remover linguagem guardando o arquivo e voltar a texto simples");
	closeDocument(currentIndex(), false);

	std::error_code executableError;
	// Development tree (linux/bin -> linux/build/plugins) or installed layout (usr/bin -> usr/lib/...).
	const auto executableRoot = std::filesystem::read_symlink("/proc/self/exe", executableError).parent_path().parent_path();
	auto pluginDirectory = executableRoot / "build" / "plugins";
	if (!std::filesystem::exists(pluginDirectory / "text-tools" / "text-tools.so", executableError))
		pluginDirectory = executableRoot / "lib" / "notepad-plus-plus-linux" / "plugins";
	scanPlugins({pluginDirectory});
	auto &plugins = _plugins.plugins();
	const auto sample = std::find_if(plugins.begin(), plugins.end(), [](const Plugin &plugin) { return plugin.id == "text-tools.so"; });
	check(sample != plugins.end() && !sample->enabled && !sample->loaded, "encontrar plugin sem carregá-lo antes de ser ativado");
	const std::size_t sampleIndex = static_cast<std::size_t>(sample - plugins.begin());
	check(enablePlugin("text-tools.so", true) && plugins[sampleIndex].loaded && plugins[sampleIndex].name == "Ferramentas de texto"
		&& plugins[sampleIndex].commands.size() == 4 && _settings.enabledPlugins == std::vector<std::string>{"text-tools.so"}, "ativar e carregar plugin nativo");
	GList *pluginItems = gtk_container_get_children(GTK_CONTAINER(_pluginMenu));
	const std::string firstPluginItem = pluginItems ? gtk_menu_item_get_label(GTK_MENU_ITEM(pluginItems->data)) : "";
	check(g_list_length(pluginItems) == 10 && firstPluginItem == "Ferramentas de texto", "mostrar comandos do plugin no menu Plugins");
	g_list_free(pluginItems);
	auto *pluginDocument = createDocument("um dois\n\ntres\n");
	pluginDocument->send(SCI_SETSEL, 0, 0);
	check(runPluginCommand(sampleIndex, 2) && pluginDocument->text() == "um dois\ntres\n", "executar comando de plugin que edita o texto");
	check(runPluginCommand(sampleIndex, 1) && pluginDocument->text() == "tres\num dois\n", "plugin usa seleção e documento atual");
	pluginDocument->send(SCI_UNDO);
	check(pluginDocument->text() == "um dois\ntres\n", "desfazer alteração feita por plugin");
	check(runPluginCommand(sampleIndex, 0) && std::string(gtk_label_get_text(GTK_LABEL(_messageLabel))).find("3 palavras") != std::string::npos, "plugin mostra mensagens pelo editor");
	pluginDocument->send(SCI_DOCUMENTEND);
	check(runPluginCommand(sampleIndex, 3) && pluginDocument->text().find('/') != std::string::npos, "plugin insere texto no cursor");
	check(!runPluginCommand(sampleIndex, 99) && !runPluginCommand(999, 0), "ignorar comandos de plugin inexistentes");
	const auto badPlugins = directory / "plugins-invalidos";
	std::filesystem::remove_all(badPlugins, error);
	error.clear();
	std::filesystem::create_directories(badPlugins, error);
	writeFile(badPlugins / "falso.so", "isto não é uma biblioteca");
	std::filesystem::copy_file(pluginDirectory / "text-tools" / "text-tools.so", badPlugins / "inseguro.so", std::filesystem::copy_options::overwrite_existing, error);
	std::filesystem::permissions(badPlugins / "inseguro.so", std::filesystem::perms::others_write, std::filesystem::perm_options::add, error);
	{
		PluginManager isolated;
		isolated.scan({badPlugins}, {"falso.so", "inseguro.so"});
		isolated.loadEnabled(&_pluginHost);
		const auto &list = isolated.plugins();
		const auto fake = std::find_if(list.begin(), list.end(), [](const Plugin &plugin) { return plugin.id == "falso.so"; });
		const auto unsafe = std::find_if(list.begin(), list.end(), [](const Plugin &plugin) { return plugin.id == "inseguro.so"; });
		check(fake != list.end() && !fake->loaded && !fake->error.empty(), "recusar arquivo que não é plugin");
		check(unsafe != list.end() && !unsafe->loaded && unsafe->error.find("permissões") != std::string::npos, "recusar plugin que qualquer usuário pode alterar");
	}
	check(enablePlugin("text-tools.so", false) && _settings.enabledPlugins.empty() && !plugins[sampleIndex].enabled, "desativar plugin");
	closeDocument(currentIndex(), false);
	while (documentCount() > 1)
		closeDocument(documentCount() - 1, false);

	// Menus equivalent to the Windows version.
	{
		std::vector<std::string> numbers = {"10", "abc", "9", "-2", "1.5"};
		sortLineList(numbers, SortKind::Integer, false);
		check(numbers == std::vector<std::string>{"abc", "1.5", "-2", "9", "10"}, "ordenar linhas como números inteiros");
		std::vector<std::string> decimals = {"1,5", "1.000,25", "-3,1"};
		sortLineList(decimals, SortKind::DecimalComma, true);
		check(decimals == std::vector<std::string>{"1.000,25", "1,5", "-3,1"}, "ordenar decimais com vírgula em ordem decrescente");
		std::vector<std::string> lengths = {"ccc", "a", "bb"};
		sortLineList(lengths, SortKind::Length, false);
		std::vector<std::string> folded = {"b", "B", "a"};
		sortLineList(folded, SortKind::LexicalIgnoreCase, false);
		check(lengths == std::vector<std::string>{"a", "bb", "ccc"} && folded == std::vector<std::string>{"a", "b", "B"}, "ordenar pelo comprimento e ignorando maiúsculas");
		std::vector<std::string> repeated = {"a", "a", "b", "a"};
		std::vector<std::string> empty = {"a", "", "  ", "b"};
		check(removeConsecutiveDuplicates(repeated) == 1 && repeated == std::vector<std::string>{"a", "b", "a"}
			&& removeEmptyLines(empty, true) == 2, "remover duplicadas consecutivas e linhas vazias");
		check(convertCase("olá mundo d'água", CaseKind::Proper) == "Olá Mundo D'água"
			&& convertCase("primeira FRASE. segunda frase", CaseKind::Sentence) == "Primeira frase. Segunda frase"
			&& convertCase("AbÇ", CaseKind::Invert) == "aBç", "converter para Primeiras Maiúsculas, frase e invertido");
		check(trimLeading("  x ") == "x " && tabsToSpaces("\tx", 4) == "    x" && spacesToTabs("        x", 4, true) == "\t\tx"
			&& spacesToTabs("a b", 4, false) == "a b", "converter espaços e tabulações");
		check(hashHex(HashKind::Md5, "abc") == "900150983cd24fb0d6963f7d28e17f72"
			&& hashHex(HashKind::Sha256, "abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "gerar MD5 e SHA-256");
		check(expandVariables("a $(X) $(Y)", {{"X", "1"}}) == "a 1 $(Y)" && shellQuote("it's") == "'it'\\''s'", "expandir variáveis do menu Executar");
		std::string bytes;
		std::string encodeError;
		std::string decoded;
		std::string detected;
		check(encodeText("ação", "UTF-16LE", bytes, encodeError) && bytes.size() == 10 && bytes.compare(0, 2, "\xFF\xFE") == 0
			&& decodeBytes(bytes, "", detected, decoded, encodeError) && detected == "UTF-16LE" && decoded == "ação", "gravar e ler UTF-16 com BOM");
		check(encodeText("ação", "WINDOWS-1252", bytes, encodeError) && bytes == "a\xe7\xe3o"
			&& !encodeText("€", "ISO-8859-1", bytes, encodeError) && !encodeError.empty(), "gravar em ANSI e recusar caractere que não cabe");
		check(decodeBytes("\x80 caf\xe9", "", detected, decoded, encodeError) && detected == "WINDOWS-1252" && decoded == "€ café"
			&& decodeBytes("\xEF\xBB\xBFoi", "", detected, decoded, encodeError) && detected == "UTF-8-BOM" && decoded == "oi", "detectar ANSI e UTF-8 com BOM");
		check(blockCommentFor("cpp").first == "/*" && blockCommentFor("html").second == "-->", "conhecer comentários em bloco");
	}
	{
		const auto utf16File = directory / "utf16.txt";
		std::string utf16;
		std::string encodeError;
		encodeText("linha ç\r\n", "UTF-16LE", utf16, encodeError);
		writeFile(utf16File, utf16);
		openPath(utf16File);
		EditorDocument *wide = currentDocument();
		check(wide && wide->encoding == "UTF-16LE" && wide->text() == "linha ç\r\n" && wide->eolMode == EolMode::Windows, "abrir arquivo UTF-16 LE");
		wide->send(SCI_DOCUMENTEND);
		wide->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("fim"));
		std::string saveError;
		std::string reread;
		std::string readEncoding;
		check(wide->save(utf16File, saveError), "salvar arquivo UTF-16 LE");
		std::ostringstream rawBuffer;
		rawBuffer << std::ifstream(utf16File, std::ios::binary).rdbuf();
		check(rawBuffer.str().compare(0, 2, "\xFF\xFE") == 0 && decodeBytes(rawBuffer.str(), "", readEncoding, reread, saveError)
			&& reread == "linha ç\r\nfim", "manter a codificação UTF-16 ao salvar");
		check(convertEncoding(*wide, "UTF-8-BOM") && wide->encoding == "UTF-8-BOM" && wide->send(SCI_GETMODIFY), "converter documento para UTF-8 com BOM");
		std::string copyError;
		check(wide->writeCopy(directory / "copia.txt", copyError) && wide->path == std::filesystem::absolute(utf16File).lexically_normal(), "salvar uma cópia sem trocar o arquivo aberto");
		std::ostringstream copyBuffer;
		copyBuffer << std::ifstream(directory / "copia.txt", std::ios::binary).rdbuf();
		check(copyBuffer.str() == "\xEF\xBB\xBFlinha ç\r\nfim", "cópia gravada em UTF-8 com BOM");
		check(reopenWithEncoding("UTF-16LE") && wide->encoding == "UTF-16LE" && !wide->send(SCI_GETMODIFY), "reabrir com outra codificação");
		closeDocument(currentIndex(), false);
	}
	{
		auto *lines = createDocument("b\n\na\n\nc\n");
		lines->send(SCI_SETSEL, 0, 0);
		perform(Action::RemoveEmptyLines);
		check(lines->text() == "b\na\nc\n", "remover linhas vazias pelo menu");
		lines->send(SCI_SETSEL, 0, 0);
		perform(Action::ReverseLines);
		check(lines->text() == "c\na\nb\n", "inverter a ordem das linhas");
		lines->setText("1\n10\n2\n");
		perform(Action::SortIntegerAsc);
		check(lines->text() == "1\n2\n10\n", "ordenar como inteiros pelo menu");
		lines->setText("um\ndois\ntres");
		lines->send(SCI_SETSEL, 0, lines->send(SCI_GETLENGTH));
		perform(Action::JoinLines);
		check(lines->text() == "um dois tres", "juntar linhas");
		lines->setText("x");
		lines->send(SCI_GOTOPOS, 0);
		perform(Action::InsertLineBelow);
		check(lines->text() == "x\n" && lines->send(SCI_LINEFROMPOSITION, lines->send(SCI_GETCURRENTPOS)) == 1, "inserir linha em branco abaixo");
		lines->setText("ola mundo");
		lines->send(SCI_SETSEL, 0, 9);
		perform(Action::ProperCase);
		check(lines->text() == "Ola Mundo", "Primeiras Maiúsculas na seleção");
		check(documentSummary(*lines).find("Palavras: 2") != std::string::npos, "resumo do documento conta palavras");
		lines->setText("  a  \n b \n");
		lines->send(SCI_SETSEL, 0, 0);
		perform(Action::TrimBoth);
		check(lines->text() == "a\nb\n", "remover espaços no início e no fim");
		lines->setText("\ta\n");
		perform(Action::TabsToSpaces);
		const bool spaced = lines->text() == std::string(static_cast<std::size_t>(_settings.tabWidth), ' ') + "a\n";
		perform(Action::SpacesToTabsLeading);
		check(spaced && lines->text() == "\ta\n", "trocar tabulação por espaços e de volta");
		lines->setText("a\nb\nc");
		lines->send(SCI_SETSEL, 0, 0);
		perform(Action::EolToSpace);
		check(lines->text() == "a b c", "transformar fins de linha em espaços");

		lines->language = &languageForPath("teste.cpp");
		applyTheme(*lines);
		lines->setText("int a;\n");
		lines->send(SCI_SETSEL, 0, lines->send(SCI_GETLENGTH));
		perform(Action::LineComment);
		const bool commented = lines->text() == "// int a;\n";
		lines->send(SCI_SETSEL, 0, lines->send(SCI_GETLENGTH));
		perform(Action::LineUncomment);
		check(commented && lines->text() == "int a;\n", "comentar e descomentar linhas");
		lines->setText("x = 1;");
		lines->send(SCI_SETSEL, 0, 6);
		perform(Action::BlockComment);
		const bool blocked = lines->text() == "/*x = 1;*/";
		lines->send(SCI_GOTOPOS, 4);
		perform(Action::BlockUncomment);
		check(blocked && lines->text() == "x = 1;", "comentário em bloco e remoção");

		lines->setText("a\nb\nc\nd\n");
		lines->send(SCI_GOTOLINE, 1);
		perform(Action::ToggleBookmark);
		lines->send(SCI_GOTOLINE, 3);
		perform(Action::ToggleBookmark);
		lines->send(SCI_GOTOLINE, 0);
		perform(Action::NextBookmark);
		const sptr_t firstMark = lines->send(SCI_LINEFROMPOSITION, lines->send(SCI_GETCURRENTPOS));
		perform(Action::NextBookmark);
		const sptr_t secondMark = lines->send(SCI_LINEFROMPOSITION, lines->send(SCI_GETCURRENTPOS));
		check(bookmarkedLineCount(*lines) == 2 && firstMark == 1 && secondMark == 3, "marcar linhas e ir ao próximo marcador");
		perform(Action::RemoveBookmarkedLines);
		check(lines->text() == "a\nc\n" && bookmarkedLineCount(*lines) == 0, "remover linhas marcadas");
		lines->send(SCI_UNDO);
		check(lines->text() == "a\nb\nc\nd\n", "desfazer remoção de linhas marcadas em um passo");

		lines->setText("foo bar foo foobar");
		lines->send(SCI_GOTOPOS, 1);
		perform(Action::StyleToken1);
		const bool styled = lines->send(SCI_INDICATORVALUEAT, 21, 0) && lines->send(SCI_INDICATORVALUEAT, 21, 8) && !lines->send(SCI_INDICATORVALUEAT, 21, 12);
		lines->send(SCI_GOTOPOS, 5);
		perform(Action::JumpStyleDown);
		const bool jumped = lines->send(SCI_GETSELECTIONSTART) == 8 && lines->send(SCI_GETSELECTIONEND) == 11;
		perform(Action::ClearAllStyles);
		check(styled && jumped && !lines->send(SCI_INDICATORVALUEAT, 21, 0), "destacar ocorrências, ir até elas e limpar");

		lines->setText("int f() {\n  return 1;\n}\nint g() {\n  return 2;\n}\n");
		perform(Action::FoldAll);
		const bool hidden = !lines->send(SCI_GETLINEVISIBLE, 1) && !lines->send(SCI_GETLINEVISIBLE, 4);
		perform(Action::UnfoldAll);
		const bool shown = lines->send(SCI_GETLINEVISIBLE, 1) && lines->send(SCI_GETLINEVISIBLE, 4);
		perform(Action::FoldLevel1);
		check(hidden && shown && !lines->send(SCI_GETLINEVISIBLE, 1), "recolher e expandir blocos de código");
		perform(Action::UnfoldAll);
		lines->send(SCI_GOTOLINE, 1);
		perform(Action::HideLines);
		const bool lineHidden = !lines->send(SCI_GETLINEVISIBLE, 1);
		perform(Action::ShowHiddenLines);
		check(lineHidden && lines->send(SCI_GETLINEVISIBLE, 1), "ocultar e mostrar linhas");

		lines->setText("f(a[1])");
		lines->send(SCI_GOTOPOS, 1);
		perform(Action::GoToMatchingBrace);
		const bool braced = lines->send(SCI_GETCURRENTPOS) == 6;
		lines->send(SCI_GOTOPOS, 4);
		perform(Action::SelectBetweenBraces);
		check(braced && lines->send(SCI_GETSELECTIONSTART) == 4 && lines->send(SCI_GETSELECTIONEND) == 5, "ir à chave correspondente e selecionar entre chaves");

		lines->setText("x y x z x");
		lines->send(SCI_GOTOPOS, 0);
		perform(Action::SelectAllMatchesCaseWord);
		check(lines->send(SCI_GETSELECTIONS) == 3, "selecionar todas as ocorrências com cursores múltiplos");
		lines->send(SCI_CLEARSELECTIONS);

		lines->setText("a\nb\nc\n");
		lines->send(SCI_SETSEL, 0, 5);
		ColumnInsert numbersInsert;
		numbersInsert.numbers = true;
		numbersInsert.initial = 8;
		numbersInsert.increment = 1;
		numbersInsert.padding = 1;
		insertColumn(*lines, numbersInsert);
		check(lines->text() == "08a\n09b\n10c\n", "editor de colunas numera linhas com zeros à esquerda");

		lines->setText("");
		std::vector<MacroCommand> macro(1);
		macro[0].message = SCI_REPLACESEL;
		macro[0].hasText = true;
		macro[0].text = "x";
		const auto macroFile = directory / "macros" / "Minha macro.xml";
		check(SessionStore::saveMacroFile(macroFile, macro) && SessionStore::loadMacroFile(macroFile).size() == 1
			&& runMacroFile(macroFile, 3, false) && lines->text() == "xxx", "salvar macro com nome e executá-la várias vezes");
		lines->setText("um\ndois\ntres\n");
		std::vector<MacroCommand> lineMacro(2);
		lineMacro[0].message = SCI_HOME;
		lineMacro[1].message = SCI_REPLACESEL;
		lineMacro[1].hasText = true;
		lineMacro[1].text = "- ";
		std::vector<MacroCommand> withDown = lineMacro;
		MacroCommand down;
		down.message = SCI_LINEDOWN;
		withDown.push_back(down);
		lines->send(SCI_GOTOPOS, 0);
		check(runMacroCommands(withDown, 1, true) && lines->text() == "- um\n- dois\n- tres\n", "executar macro até o fim do arquivo");

		lines->setText("aa bb aa");
		lines->send(SCI_GOTOPOS, 0);
		perform(Action::StyleOneToken2);
		lines->send(SCI_GOTOPOS, 3);
		perform(Action::StyleToken3);
		check(styledText(*lines, 1) == "aa" && styledText(*lines, 2) == "bb" && styledText(*lines, -1) == "aa\nbb", "destacar um token e copiar texto destacado");
		perform(Action::ClearAllStyles);
		lines->setText("abcdef");
		lines->send(SCI_GOTOPOS, 1);
		perform(Action::BeginEndSelect);
		lines->send(SCI_GOTOPOS, 4);
		perform(Action::BeginEndSelect);
		check(lines->send(SCI_GETSELECTIONSTART) == 1 && lines->send(SCI_GETSELECTIONEND) == 4 && lines->selectionMark < 0, "iniciar e terminar seleção");
		lines->setText("xy ab xy");
		gtk_entry_set_text(GTK_ENTRY(_searchEntry), "zzz");
		lines->send(SCI_SETSEL, 0, 2);
		perform(Action::FindVolatileNext);
		check(lines->send(SCI_GETSELECTIONSTART) == 6 && std::string(gtk_entry_get_text(GTK_ENTRY(_searchEntry))) == "zzz", "localizar (volátil) sem mudar a caixa de busca");
		gtk_entry_set_text(GTK_ENTRY(_searchEntry), "");
		lines->setText("a\nb\nc\n");
		toggleBookmark(*lines, 0);
		toggleBookmark(*lines, 2);
		copyToClipboard("X\nY");
		perform(Action::PasteReplaceBookmarked);
		check(lines->text() == "X\nb\nY\n", "colar substituindo linhas marcadas");
		perform(Action::ClearBookmarks);

		// Built-in features of the Windows plugins.
		std::string decodedText;
		check(base64Encode("Olá") == "T2zDoQ==" && base64Decode("T2zDoQ==", decodedText) && decodedText == "Olá"
			&& !base64Decode("@@", decodedText), "codificar e decodificar Base64");
		check(urlEncode("a b/ç") == "a%20b%2F%C3%A7" && urlDecode("a+b%2F%C3%A7", decodedText) && decodedText == "a b/ç", "codificar e decodificar URL");
		check(asciiToHex("Hi") == "4869" && hexToAscii("0x48 69", decodedText) && decodedText == "Hi" && !hexToAscii("4G", decodedText), "converter ASCII e hexadecimal");
		std::string minified;
		std::string minifyError;
		check(jsMinify("// nota\nfunction soma(a, b) {\n  return a + b; /* fim */\n}\nvar s = \"x  y\";\n", minified, minifyError)
			&& minified == "function soma(a,b){return a+b;}\nvar s=\"x  y\";", "minificar JavaScript como o JSMin");
		check(!jsMinify("var s = \"sem fim", minified, minifyError) && !minifyError.empty(), "avisar texto sem fim ao minificar");
		check(jsFormat("function f(){if(a){b();}return 1;}", "  ") == "function f(){\n  if(a){\n    b();\n  }\n  return 1;\n}\n", "formatar JavaScript");
		const auto diff = diffLines(splitLines("a\nb\nc\nd\n"), splitLines("a\nB\nc\nd\ne\n"));
		const std::string unified = unifiedDiff(diff, "esq", "dir");
		check(unified.find("-b\n+B\n") != std::string::npos && unified.find("+e\n") != std::string::npos
			&& unified.rfind("--- esq\n+++ dir\n@@ -1,4 +1,5 @@", 0) == 0, "comparar linhas e gerar diferenças");
		auto *leftDoc = createDocument("a\nb\nc\n");
		leftDoc->displayName = "esquerda";
		auto *rightDoc = createDocument("a\nx\nc\nnova\n");
		rightDoc->displayName = "direita";
		const std::size_t before = documentCount();
		const std::size_t blocks = compareDocuments(*leftDoc, *rightDoc, true);
		check(blocks == 2 && (leftDoc->send(SCI_MARKERGET, 1) & (1 << 15)) && (rightDoc->send(SCI_MARKERGET, 1) & (1 << 16))
			&& (rightDoc->send(SCI_MARKERGET, 3) & (1 << 16)) && !(leftDoc->send(SCI_MARKERGET, 0) & (1 << 15))
			&& documentCount() == before, "comparar dois documentos abertos");
		// Side by side: same number of rows on both sides, "b"/"x" on the same
		// row, an empty grey row on the left in front of "nova".
		const auto paneLine = [](EditorDocument &pane, sptr_t line) {
			std::string text(static_cast<std::size_t>(pane.send(SCI_LINELENGTH, static_cast<uptr_t>(line))), '\0');
			pane.send(SCI_GETLINE, static_cast<uptr_t>(line), reinterpret_cast<sptr_t>(text.data()));
			while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
				text.pop_back();
			return text;
		};
		const bool sideBySide = _compareWindow && _compareLeft && _compareRight
			&& _compareLeft->send(SCI_GETLINECOUNT) == 4 && _compareRight->send(SCI_GETLINECOUNT) == 4
			&& paneLine(*_compareLeft, 1) == "b" && paneLine(*_compareRight, 1) == "x"
			&& paneLine(*_compareLeft, 3).empty() && paneLine(*_compareRight, 3) == "nova"
			&& (_compareLeft->send(SCI_MARKERGET, 1) & (1 << 20)) && (_compareRight->send(SCI_MARKERGET, 1) & (1 << 21))
			&& (_compareLeft->send(SCI_MARKERGET, 3) & (1 << 22)) && !(_compareLeft->send(SCI_MARKERGET, 0) & (7 << 20))
			&& _compareLeft->send(SCI_GETREADONLY) && _compareBlocks.size() == 2 && _compareCaretRow == 1;
		check(sideBySide, "comparação lado a lado com linhas alinhadas");
		compareJump(true);
		const bool compareJumped = _compareCaretRow == 3 && _compareRight->send(SCI_LINEFROMPOSITION, _compareRight->send(SCI_GETCURRENTPOS)) == 3;
		compareJump(true);
		check(compareJumped && _compareCaretRow == 1, "próxima diferença na comparação lado a lado");
		openCompareReport();
		check(documentCount() == before + 1 && currentDocument()->language->id == "diff"
			&& currentDocument()->text().find("-b\n+x\n") != std::string::npos, "relatório diff da comparação");
		closeDocument(currentIndex(), false);
		const WordChanges words = wordDiff("int total = soma(a, 1);", "int total = soma(a, 22);");
		check(words.left.size() == 1 && words.left[0] == std::make_pair<std::size_t, std::size_t>(20, 1)
			&& words.right.size() == 1 && words.right[0] == std::make_pair<std::size_t, std::size_t>(20, 2)
			&& wordDiff("abc", "xyz").left.empty(), "destacar as palavras alteradas");
		leftDoc->setText("int x = 1;\nfim\n");
		rightDoc->setText("int x = 2;\nfim\n");
		compareDocuments(*leftDoc, *rightDoc, true);
		const sptr_t wordStart = _compareLeft->send(SCI_INDICATORSTART, 28, 8);
		check(_compareLeft->send(SCI_INDICATORVALUEAT, 28, 8) && !_compareLeft->send(SCI_INDICATORVALUEAT, 28, 4)
			&& _compareRight->send(SCI_INDICATORVALUEAT, 29, 8) && wordStart == 8, "cores nas palavras alteradas lado a lado");
		_compareIgnoreSpaces = true;
		rightDoc->setText("int  x =  1;\nfim\n");
		check(compareDocuments(*leftDoc, *rightDoc, true) == 0 && _compareBlocks.empty(), "ignorar espaços na comparação");
		_compareIgnoreSpaces = false;
		compareDocumentClosed(rightDoc);
		check(!_compareRightSource && !_compareLeftSource, "fechar um documento comparado");
		gtk_widget_destroy(_compareWindow);
		check(!_compareWindow && !_compareLeft, "fechar a janela de comparação");
		leftDoc->setText("a\nb\nc\n");
		rightDoc->setText("a\nx\nc\nnova\n");
		compareDocuments(*leftDoc, *rightDoc, false);
		setCurrentPage(static_cast<int>(documentCount()) - 1);
		rightDoc->send(SCI_GOTOLINE, 0);
		perform(Action::CompareNext);
		const bool nextDiff = rightDoc->send(SCI_LINEFROMPOSITION, rightDoc->send(SCI_GETCURRENTPOS)) == 1;
		perform(Action::CompareClear);
		check(nextDiff && !(rightDoc->send(SCI_MARKERGET, 1) & (1 << 16)), "ir à próxima diferença e limpar comparação");
		check(compareDocuments(*leftDoc, *leftDoc, false) == 0, "documentos iguais não têm diferenças");
		closeDocument(currentIndex(), false);
		setCurrentPage(static_cast<int>(documentCount()) - 1);
		closeDocument(currentIndex(), false);
		lines = currentDocument();
		lines->setText("ab cd");
		lines->send(SCI_SETSEL, 3, 5);
		perform(Action::AsciiToHex);
		const bool hexed = lines->text() == "ab 6364";
		lines->send(SCI_SETSEL, 0, 0);
		perform(Action::Base64Encode);
		check(hexed && lines->text() == base64Encode("ab 6364"), "MIME Tools e conversor na seleção ou no documento");
		lines->language = &languageForPath("teste.cpp");
		applyTheme(*lines);
		lines->setText("int x = 1; // <nota> ç\n");
		const std::string html = documentHtml(*lines);
		const std::string rtf = documentRtf(*lines);
		check(html.find("&lt;nota&gt;") != std::string::npos && html.find("<span class=\"s") != std::string::npos && html.find("color: #") != std::string::npos
			&& rtf.rfind("{\\rtf1", 0) == 0 && rtf.find("\\colortbl") != std::string::npos && rtf.find("\\u231?") != std::string::npos, "exportar para HTML e RTF com cores");

		check(shortcutListText().find("Ctrl+N") != std::string::npos && shortcutListText().find("Arquivo → Novo") != std::string::npos, "listar atalhos de teclado dos menus");
		GList *topMenus = gtk_container_get_children(GTK_CONTAINER(_menuBar));
		std::string menuNames;
		for (GList *entry = topMenus; entry; entry = entry->next)
		{
			std::string label = gtk_menu_item_get_label(GTK_MENU_ITEM(entry->data));
			label.erase(std::remove(label.begin(), label.end(), '_'), label.end());
			menuNames += label + "|";
		}
		g_list_free(topMenus);
		check(menuNames == "Arquivo|Editar|Pesquisar|Exibir|Codificação|Linguagem|Configurações|Ferramentas|Macro|Executar|Plugins|Janela|Ajuda|",
			"menus na mesma ordem da versão Windows");
		check(!quitKeepsUnsaved(), "teste não grava sessão ao sair");
		closeDocument(currentIndex(), false);
	}
	{
		while (documentCount() > 0)
			closeDocument(documentCount() - 1, false);
		auto *b = createDocument("2");
		b->displayName = "b.txt";
		auto *a = createDocument("1");
		a->displayName = "a.txt";
		auto *c = createDocument("33");
		c->displayName = "c.txt";
		perform(Action::SortTabsNameAsc);
		const bool sorted = _documents[0].get() == a && _documents[1].get() == b && _documents[2].get() == c;
		perform(Action::SortTabsSizeDesc);
		const bool bySize = _documents[0].get() == c;
		setCurrentPage(0);
		perform(Action::MoveTabEnd);
		check(sorted && bySize && _documents[2].get() == c && currentDocument() == c, "ordenar e mover abas");
		perform(Action::FirstTab);
		const bool first = currentIndex() == 0;
		perform(Action::PreviousTab);
		check(first && currentIndex() == 2, "navegar entre abas pelo menu");
		b->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("mudou"));
		perform(Action::CloseAllUnchanged);
		check(documentCount() == 1 && _documents[0].get() == b, "fechar todos os documentos inalterados");
		std::filesystem::remove(directory / "s2.txt", error);
		writeFile(directory / "s1.txt", "s1");
		openPath(directory / "s1.txt");
		const auto sessionFile = directory / "sessao.xml";
		const std::string sessionPathText = currentDocument()->path.string();
		check(saveSessionFileAs(sessionFile) && SessionStore::loadSessionFile(sessionFile) == std::vector<std::string>{sessionPathText},
			"salvar e ler sessão no formato do Notepad++");
		closeDocument(currentIndex(), false);
		check(openSessionFile(sessionFile) && currentDocument() && currentDocument()->displayName == "s1.txt", "carregar sessão salva");
		std::ofstream(directory / "s1.txt", std::ios::app) << "\nnova linha";
		check(reloadFromDisk(*currentDocument(), false) && currentDocument()->text() == "s1\nnova linha", "recarregar arquivo do disco");
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_monitoringCheck), TRUE);
		const bool monitoring = currentDocument()->monitoring && currentDocument()->send(SCI_GETREADONLY);
		std::ofstream(directory / "s1.txt", std::ios::app) << "\nmais uma";
		monitorTick();
		const bool followed = currentDocument()->text() == "s1\nnova linha\nmais uma";
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(_monitoringCheck), FALSE);
		check(monitoring && followed && !currentDocument()->monitoring && !currentDocument()->send(SCI_GETREADONLY), "monitorar arquivo (tail -f)");
		check(renameDocument(*currentDocument(), "s2.txt") && std::filesystem::is_regular_file(directory / "s2.txt")
			&& !std::filesystem::exists(directory / "s1.txt"), "renomear arquivo aberto");
		closeDocument(currentIndex(), false);
		perform(Action::RestoreClosedFile);
		check(currentDocument() && currentDocument()->displayName == "s2.txt", "restaurar arquivo fechado recentemente");
		perform(Action::ClearRecentList);
		check(_settings.recentFiles.empty(), "limpar lista de arquivos recentes");
		while (documentCount() > 1)
			closeDocument(documentCount() - 1, false);
	}

	{
		auto labels = [](GtkWidget *menu) {
			std::string all;
			GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
			for (GList *entry = children; entry; entry = entry->next)
			{
				if (GTK_IS_MENU_ITEM(entry->data) && !GTK_IS_SEPARATOR_MENU_ITEM(entry->data))
				{
					std::string label = gtk_menu_item_get_label(GTK_MENU_ITEM(entry->data));
					label.erase(std::remove(label.begin(), label.end(), '_'), label.end());
					all += label + "|";
				}
			}
			g_list_free(children);
			return all;
		};
		check(labels(_editorContextMenu).find("Recortar|Copiar|Colar|") == 0 && labels(_editorContextMenu).find("Comentário em bloco") != std::string::npos
			&& labels(_tabContextMenu).find("Fechar|Fechar todos exceto este|") == 0 && labels(_tabContextMenu).find("Copiar caminho completo") != std::string::npos,
			"menus do botão direito no texto e nas abas");

		const auto external = directory / "externo.txt";
		writeFile(external, "um");
		openPath(external);
		EditorDocument *watched = currentDocument();
		writeFile(external, "dois");
		check(checkExternalChanges(true) && watched->text() == "dois" && !watched->send(SCI_GETMODIFY), "recarregar arquivo alterado por outro programa");
		watched->send(SCI_DOCUMENTEND);
		watched->send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("!"));
		writeFile(external, "tres e mais");
		check(checkExternalChanges(true) && watched->text() == "dois!" && watched->send(SCI_GETMODIFY), "não recarregar por cima de alterações não salvas sem perguntar");
		check(!checkExternalChanges(true), "não avisar de novo sobre a mesma alteração");
		std::filesystem::remove(external, error);
		check(checkExternalChanges(true) && watched->missingNotified && watched->send(SCI_GETMODIFY) && watched->text() == "dois!", "avisar arquivo apagado e manter o texto");
		check(!checkExternalChanges(true), "avisar uma só vez sobre arquivo apagado");
		closeDocument(currentIndex(), false);

		const auto socket = std::filesystem::absolute(directory) / "instancia.sock";
		writeFile(directory / "ipc.txt", "ipc");
		check(!sendToRunningInstance(socket, {}), "sem editor aberto, o novo início abre a própria janela");
		const bool listening = startInstanceServer(socket);
		const bool sent = sendToRunningInstance(socket, {directory / "ipc.txt"});
		const gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
		while (g_get_monotonic_time() < until && !(currentDocument() && currentDocument()->displayName == "ipc.txt"))
		{
			if (!g_main_context_iteration(nullptr, FALSE))
				g_usleep(1000);
		}
		check(listening && sent && currentDocument() && currentDocument()->displayName == "ipc.txt", "segundo início entrega o arquivo ao editor já aberto");
		stopInstanceServer();
		check(!std::filesystem::exists(socket), "liberar o canal ao fechar o editor");
		closeDocument(currentIndex(), false);
	}
	{
		// Find / Replace / Mark dialog like the Windows version.
		auto *found = createDocument("um dois um\nTRES um\n");
		found->send(SCI_SETSEL, 0, 0);
		showFindDialog(0);
		auto setText = [](GtkWidget *combo, const char *text) { gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), text); };
		auto setActive = [](GtkWidget *toggle, bool value) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), value); };
		auto status = [&]() { return std::string(gtk_label_get_text(GTK_LABEL(_dlgStatus))); };
		check(_findWindow && gtk_notebook_get_n_pages(GTK_NOTEBOOK(_findTabs)) == 4, "diálogo Localizar com abas Localizar, Substituir, Marcar e Em arquivos");
		setText(_dlgFind, "um");
		check(runFindOperation(FindOperation::Count) && status().find("3 ocorrência") != std::string::npos, "contar ocorrências pelo diálogo");
		setActive(_dlgExtended, true);
		setText(_dlgFind, "\\n");
		check(runFindOperation(FindOperation::Count) && status().find("2 ocorrência") != std::string::npos, "modo de busca estendido (\\n)");
		setActive(_dlgNormal, true);
		setText(_dlgFind, "um");
		found->send(SCI_SETSEL, 0, 10);
		setActive(_dlgInSelection, true);
		setText(_dlgReplace, "1");
		check(runFindOperation(FindOperation::ReplaceAll) && found->text() == "1 dois 1\nTRES um\n", "substituir tudo só na seleção");
		setActive(_dlgInSelection, false);
		found->send(SCI_SETSEL, 0, 0);
		setActive(_dlgBookmarkLine, true);
		setActive(_dlgPurge, true);
		check(runFindOperation(FindOperation::MarkAll) && styledText(*found, 5) == "um" && (found->send(SCI_MARKERGET, 1) & (1 << 20))
			&& !(found->send(SCI_MARKERGET, 0) & (1 << 20)), "marcar tudo e marcar as linhas na margem esquerda");
		check(runFindOperation(FindOperation::CopyMarked) && runFindOperation(FindOperation::ClearMarks) && styledText(*found, 5).empty(), "copiar texto marcado e limpar marcas");
		found->send(SCI_MARKERDELETEALL, 20);
		setText(_dlgFind, "1");
		check(runFindOperation(FindOperation::FindAllCurrent) && _results.size() == 2, "localizar tudo no documento atual");
		setActive(_dlgRegex, true);
		setText(_dlgFind, "d.is");
		found->send(SCI_SETSEL, 0, 0);
		check(runFindOperation(FindOperation::Next) && found->send(SCI_GETSELECTIONSTART) == 2 && found->send(SCI_GETSELECTIONEND) == 6, "localizar com expressão regular");
		setActive(_dlgNormal, true);
		setText(_dlgFind, "TRES");
		found->send(SCI_DOCUMENTEND);
		setActive(_dlgBackward, true);
		const bool backward = runFindOperation(FindOperation::Next) && found->send(SCI_GETSELECTIONSTART) == 9;
		setActive(_dlgBackward, false);
		found->send(SCI_SETSEL, 0, 0);
		perform(Action::FindNext);
		check(backward && found->send(SCI_GETSELECTIONSTART) == 9, "localizar para trás e F3 repete a busca do diálogo");
		setText(_dlgFind, "dois");
		setText(_dlgReplace, "2");
		found->send(SCI_SETSEL, 0, 0);
		runFindOperation(FindOperation::Replace);
		check(runFindOperation(FindOperation::Replace) && found->text() == "1 2 1\nTRES um\n", "substituir um por vez");
		_dialogSearchActive = false;
		updateTab(*found);
		const std::string shortName = found->displayName;
		GtkLabel *tabLabel = GTK_LABEL(g_object_get_data(G_OBJECT(found->widget()), "npp-tab-label"));
		const int shortWidth = gtk_label_get_width_chars(tabLabel);
		found->displayName = std::string(80, 'x') + ".txt";
		updateTab(*found);
		check(shortWidth >= static_cast<int>(g_utf8_strlen(shortName.c_str(), -1)) && gtk_label_get_width_chars(tabLabel) == 30,
			"abas mostram o nome inteiro, com limite para nomes muito longos");
		closeDocument(currentIndex(), false);
	}
	{
		// Clicks in the left margin, in a window that is never shown on screen.
		GtkWidget *offscreen = gtk_offscreen_window_new();
		gtk_window_set_default_size(GTK_WINDOW(offscreen), 600, 300);
		GtkWidget *marginWidget = scintilla_new();
		EditorDocument marginDocument(marginWidget);
		attachData(marginWidget, "npp-document", &marginDocument);
		g_signal_connect(marginWidget, SCINTILLA_NOTIFY, G_CALLBACK(notificationReceived), this);
		gtk_container_add(GTK_CONTAINER(offscreen), marginWidget);
		gtk_widget_show_all(offscreen);
		marginDocument.setText("primeira\nsegunda\nterceira\nquarta\n");
		applyTheme(marginDocument);
		applyViewOptions(marginDocument);
		while (gtk_events_pending())
			gtk_main_iteration();
		const int lineHeight = static_cast<int>(marginDocument.send(SCI_TEXTHEIGHT, 0));
		auto click = [&](double x, int line, bool release, guint state = 0) {
			GdkEvent *event = gdk_event_new(release ? GDK_BUTTON_RELEASE : GDK_BUTTON_PRESS);
			event->button.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(marginWidget)));
			event->button.x = x;
			event->button.y = line * lineHeight + lineHeight / 2.0;
			event->button.button = 1;
			event->button.state = state;
			event->button.time = static_cast<guint32>(g_get_monotonic_time() / 1000);
			gdk_event_set_device(event, gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())));
			gtk_widget_event(marginWidget, event);
			gdk_event_free(event);
		};
		const double numbersX = 10;
		click(numbersX, 1, false);
		click(numbersX, 1, true);
		const sptr_t lineStart = marginDocument.send(SCI_POSITIONFROMLINE, 1);
		const bool oneLine = marginDocument.send(SCI_GETSELECTIONSTART) == lineStart
			&& marginDocument.send(SCI_GETSELECTIONEND) == marginDocument.send(SCI_POSITIONFROMLINE, 2);
		click(numbersX, 3, false, GDK_SHIFT_MASK);
		click(numbersX, 3, true, GDK_SHIFT_MASK);
		const bool extended = marginDocument.send(SCI_GETSELECTIONSTART) == lineStart
			&& marginDocument.send(SCI_GETSELECTIONEND) == marginDocument.send(SCI_POSITIONFROMLINE, 4);
		check(oneLine && extended, "clicar na margem esquerda seleciona a linha e Shift+clique estende");
		const double bookmarkX = marginDocument.send(SCI_GETMARGINWIDTHN, 0) + 4.0;
		click(bookmarkX, 2, false);
		click(bookmarkX, 2, true);
		check((marginDocument.send(SCI_MARKERGET, 2) & (1 << 20)) != 0, "clicar na margem de marcadores marca a linha");
		gtk_widget_destroy(offscreen);
	}
	{
		// Older settings with "restore open files" off must still keep unsaved text on exit.
		writeFile(_settingsStore.path(), "<?xml version=\"1.0\"?>\n<NotepadPlusPlusLinux version=\"1\">\n\t<restoreSession>false</restoreSession>\n</NotepadPlusPlusLinux>\n");
		const Settings old = _settingsStore.load();
		writeFile(_settingsStore.path(), "<?xml version=\"1.0\"?>\n<NotepadPlusPlusLinux version=\"1\">\n\t<restoreSession>false</restoreSession>\n\t<rememberUnsaved>false</rememberUnsaved>\n</NotepadPlusPlusLinux>\n");
		const Settings off = _settingsStore.load();
		check(old.rememberUnsaved && old.restoreSession && !off.rememberUnsaved && !off.restoreSession,
			"guardar textos ao sair liga a restauração da sessão, como no Windows");
	}

	if (_selfTesting)
	{
		std::error_code cleanupError;
		std::filesystem::remove_all(_settingsStore.path().parent_path(), cleanupError);
	}
	const std::string completed = log.str();
	success = completed.find("FAIL:") == std::string::npos;
	result = completed + (success ? "Todos os testes do editor nativo passaram.\n" : "Houve falhas nos testes.\n");
	return success;
}
}
