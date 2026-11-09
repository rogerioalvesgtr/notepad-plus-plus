// The Find / Replace / Mark dialog, laid out like the one of the Windows
// version: tabs, search history, direction, whole word, match case, wrap
// around, in selection, Normal / Extended / Regular expression modes,
// transparency, count, find all, replace all (also in all open documents),
// mark all with bookmarks, and copy marked text.
#include "EditorWindow.h"

#include "BuiltinTools.h"
#include "Scintilla.h"

namespace NppLinux
{
namespace
{
constexpr int FindMarkIndicator = 26;
constexpr int FindMarkStyle = 5; // styledText() index of the mark indicator
constexpr int BookmarkMarker = 20;

std::string comboText(GtkWidget *combo)
{
	return gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))));
}

void setComboText(GtkWidget *combo, const std::string &text)
{
	gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), text.c_str());
}

bool active(GtkWidget *toggle)
{
	return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
}

sptr_t nextStart(EditorDocument &document, sptr_t found, sptr_t end)
{
	return end > found ? end : document.send(SCI_POSITIONAFTER, found);
}

std::size_t countRange(EditorDocument &document, const std::string &needle, int flags, sptr_t start, sptr_t end)
{
	std::size_t count = 0;
	document.send(SCI_SETSEARCHFLAGS, flags);
	while (start < end && count < 10000000)
	{
		document.send(SCI_SETTARGETRANGE, start, end);
		const sptr_t found = document.send(SCI_SEARCHINTARGET, needle.size(), reinterpret_cast<sptr_t>(needle.data()));
		if (found < 0)
			break;
		++count;
		const sptr_t next = nextStart(document, found, document.send(SCI_GETTARGETEND));
		if (next <= start)
			break;
		start = next;
	}
	return count;
}

std::size_t replaceRange(EditorDocument &document, const std::string &needle, const std::string &replacement,
	int flags, bool regex, sptr_t start, sptr_t &end)
{
	std::size_t count = 0;
	document.send(SCI_SETSEARCHFLAGS, flags);
	document.send(SCI_BEGINUNDOACTION);
	while (start <= end && count < 10000000)
	{
		document.send(SCI_SETTARGETRANGE, start, end);
		const sptr_t found = document.send(SCI_SEARCHINTARGET, needle.size(), reinterpret_cast<sptr_t>(needle.data()));
		if (found < 0)
			break;
		const sptr_t matchEnd = document.send(SCI_GETTARGETEND);
		const sptr_t written = document.send(regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET, replacement.size(),
			reinterpret_cast<sptr_t>(replacement.data()));
		end += written - (matchEnd - found);
		++count;
		start = found + written;
		if (matchEnd == found)
		{
			// An empty match: step over one character so the loop moves on.
			if (start >= end)
				break;
			start = document.send(SCI_POSITIONAFTER, start);
		}
	}
	document.send(SCI_ENDUNDOACTION);
	return count;
}

std::size_t markRange(EditorDocument &document, const std::string &needle, int flags, sptr_t start, sptr_t end, bool bookmark)
{
	std::size_t count = 0;
	document.send(SCI_SETSEARCHFLAGS, flags);
	document.send(SCI_SETINDICATORCURRENT, FindMarkIndicator);
	while (start < end && count < 10000000)
	{
		document.send(SCI_SETTARGETRANGE, start, end);
		const sptr_t found = document.send(SCI_SEARCHINTARGET, needle.size(), reinterpret_cast<sptr_t>(needle.data()));
		if (found < 0)
			break;
		const sptr_t matchEnd = document.send(SCI_GETTARGETEND);
		if (matchEnd > found)
			document.send(SCI_INDICATORFILLRANGE, found, matchEnd - found);
		if (bookmark)
		{
			const sptr_t line = document.send(SCI_LINEFROMPOSITION, found);
			if (!(document.send(SCI_MARKERGET, line) & (1 << BookmarkMarker)))
				document.send(SCI_MARKERADD, line, BookmarkMarker);
		}
		++count;
		const sptr_t next = nextStart(document, found, matchEnd);
		if (next <= start)
			break;
		start = next;
	}
	return count;
}

void findButtonClicked(GtkButton *button, gpointer data)
{
	const int operation = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "npp-find-operation"));
	static_cast<EditorWindow *>(data)->runFindOperation(static_cast<EditorWindow::FindOperation>(operation));
}
}

void EditorWindow::rememberFindText(GtkWidget *combo, const std::string &text)
{
	if (text.empty())
		return;
	GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
	GtkTreeIter iter;
	int index = 0;
	bool valid = gtk_tree_model_get_iter_first(model, &iter);
	while (valid)
	{
		gchar *value = nullptr;
		gtk_tree_model_get(model, &iter, 0, &value, -1);
		const bool same = value && text == value;
		g_free(value);
		if (same)
		{
			gtk_combo_box_text_remove(GTK_COMBO_BOX_TEXT(combo), index);
			break;
		}
		valid = gtk_tree_model_iter_next(model, &iter);
		++index;
	}
	gtk_combo_box_text_prepend_text(GTK_COMBO_BOX_TEXT(combo), text.c_str());
	while (gtk_tree_model_iter_n_children(model, nullptr) > 15)
		gtk_combo_box_text_remove(GTK_COMBO_BOX_TEXT(combo), 15);
	setComboText(combo, text);
}

void EditorWindow::buildFindDialog()
{
	if (_findWindow)
		return;
	_findWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(_findWindow), "Localizar");
	gtk_window_set_transient_for(GTK_WINDOW(_findWindow), GTK_WINDOW(_window));
	gtk_window_set_destroy_with_parent(GTK_WINDOW(_findWindow), TRUE);
	gtk_window_set_type_hint(GTK_WINDOW(_findWindow), GDK_WINDOW_TYPE_HINT_DIALOG);
	g_signal_connect(_findWindow, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
	g_signal_connect(_findWindow, "key-press-event", G_CALLBACK(+[](GtkWidget *widget, GdkEventKey *event, gpointer) -> gboolean {
		if (event->keyval != GDK_KEY_Escape)
			return FALSE;
		gtk_widget_hide(widget);
		return TRUE;
	}), nullptr);
	auto transparency = +[](GtkWidget *, GdkEvent *event, gpointer data) -> gboolean {
		auto *window = static_cast<EditorWindow *>(data);
		const bool focused = event->type == GDK_FOCUS_CHANGE && reinterpret_cast<GdkEventFocus *>(event)->in;
		window->applyFindTransparency(focused);
		return FALSE;
	};
	g_signal_connect(_findWindow, "focus-in-event", G_CALLBACK(transparency), this);
	g_signal_connect(_findWindow, "focus-out-event", G_CALLBACK(transparency), this);

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
	gtk_container_add(GTK_CONTAINER(_findWindow), outer);
	_findTabs = gtk_notebook_new();
	for (const char *title : {"Localizar", "Substituir", "Marcar", "Localizar em arquivos"})
		gtk_notebook_append_page(GTK_NOTEBOOK(_findTabs), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), gtk_label_new(title));
	gtk_notebook_set_show_border(GTK_NOTEBOOK(_findTabs), FALSE);
	g_signal_connect(_findTabs, "switch-page", G_CALLBACK(+[](GtkNotebook *, GtkWidget *, guint page, gpointer data) {
		auto *window = static_cast<EditorWindow *>(data);
		if (page == 3)
		{
			// The Windows dialog has Find in Files as a tab; here it opens its own window.
			g_idle_add(+[](gpointer target) -> gboolean {
				auto *editor = static_cast<EditorWindow *>(target);
				gtk_notebook_set_current_page(GTK_NOTEBOOK(editor->_findTabs), editor->_findDialogTab);
				editor->openFindInFilesFromDialog();
				return G_SOURCE_REMOVE;
			}, window);
			return;
		}
		window->updateFindDialogTab(static_cast<int>(page));
	}), this);
	gtk_box_pack_start(GTK_BOX(outer), _findTabs, FALSE, FALSE, 0);

	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	gtk_box_pack_start(GTK_BOX(body), grid, TRUE, TRUE, 0);
	auto caption = [](const char *text) {
		GtkWidget *label = gtk_label_new_with_mnemonic(text);
		gtk_label_set_xalign(GTK_LABEL(label), 1.0);
		return label;
	};
	GtkWidget *findLabel = caption("_Localizar:");
	_dlgFind = gtk_combo_box_text_new_with_entry();
	gtk_widget_set_hexpand(_dlgFind, TRUE);
	gtk_entry_set_width_chars(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(_dlgFind))), 36);
	gtk_label_set_mnemonic_widget(GTK_LABEL(findLabel), _dlgFind);
	gtk_grid_attach(GTK_GRID(grid), findLabel, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), _dlgFind, 1, 0, 1, 1);
	_dlgReplaceLabel = caption("Substituir p_or:");
	_dlgReplace = gtk_combo_box_text_new_with_entry();
	gtk_label_set_mnemonic_widget(GTK_LABEL(_dlgReplaceLabel), _dlgReplace);
	gtk_grid_attach(GTK_GRID(grid), _dlgReplaceLabel, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), _dlgReplace, 1, 1, 1, 1);
	_dlgSwap = gtk_button_new_with_label("⇅");
	gtk_widget_set_tooltip_text(_dlgSwap, "Trocar o texto a localizar pelo de substituição");
	g_signal_connect(_dlgSwap, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
		auto *window = static_cast<EditorWindow *>(data);
		const std::string find = comboText(window->_dlgFind);
		setComboText(window->_dlgFind, comboText(window->_dlgReplace));
		setComboText(window->_dlgReplace, find);
	}), this);
	gtk_grid_attach(GTK_GRID(grid), _dlgSwap, 2, 0, 1, 2);
	auto activate = +[](GtkEntry *, gpointer data) {
		auto *window = static_cast<EditorWindow *>(data);
		window->runFindOperation(window->_findDialogTab == 2 ? FindOperation::MarkAll : FindOperation::Next);
	};
	g_signal_connect(gtk_bin_get_child(GTK_BIN(_dlgFind)), "activate", G_CALLBACK(activate), this);
	g_signal_connect(gtk_bin_get_child(GTK_BIN(_dlgReplace)), "activate", G_CALLBACK(activate), this);

	_dlgBookmarkLine = gtk_check_button_new_with_mnemonic("Marcar a _linha (marcador na margem esquerda)");
	_dlgPurge = gtk_check_button_new_with_mnemonic("Limpar as marcas a cada _busca");
	gtk_grid_attach(GTK_GRID(grid), _dlgBookmarkLine, 1, 2, 2, 1);
	gtk_grid_attach(GTK_GRID(grid), _dlgPurge, 1, 3, 2, 1);

	GtkWidget *options = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(options), 16);
	_dlgBackward = gtk_check_button_new_with_mnemonic("Para _trás");
	_dlgWholeWord = gtk_check_button_new_with_mnemonic("Somente _palavra inteira");
	_dlgMatchCase = gtk_check_button_new_with_mnemonic("_Diferenciar maiúsculas");
	_dlgWrap = gtk_check_button_new_with_mnemonic("C_ircular");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_dlgWrap), TRUE);
	_dlgInSelection = gtk_check_button_new_with_mnemonic("Na _seleção");
	gtk_grid_attach(GTK_GRID(options), _dlgBackward, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(options), _dlgWholeWord, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(options), _dlgMatchCase, 0, 2, 1, 1);
	gtk_grid_attach(GTK_GRID(options), _dlgWrap, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(options), _dlgInSelection, 1, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), options, 1, 4, 2, 1);

	GtkWidget *modeFrame = gtk_frame_new("Modo de busca");
	GtkWidget *modes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_container_set_border_width(GTK_CONTAINER(modes), 6);
	_dlgNormal = gtk_radio_button_new_with_mnemonic(nullptr, "_Normal");
	_dlgExtended = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(_dlgNormal), "_Estendido (\\n, \\r, \\t, \\0, \\x...)");
	_dlgRegex = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(_dlgNormal), "E_xpressão regular");
	for (GtkWidget *mode : {_dlgNormal, _dlgExtended, _dlgRegex})
		gtk_box_pack_start(GTK_BOX(modes), mode, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(modeFrame), modes);

	GtkWidget *transparencyFrame = gtk_frame_new("Transparência");
	GtkWidget *transparencyBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_container_set_border_width(GTK_CONTAINER(transparencyBox), 6);
	_dlgTransparent = gtk_check_button_new_with_mnemonic("_Ligada");
	GtkWidget *onLoseFocus = gtk_radio_button_new_with_label(nullptr, "Ao perder o foco");
	_dlgTransparentAlways = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(onLoseFocus), "Sempre");
	_dlgTransparency = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 20, 100, 5);
	gtk_range_set_value(GTK_RANGE(_dlgTransparency), 70);
	gtk_scale_set_draw_value(GTK_SCALE(_dlgTransparency), FALSE);
	for (GtkWidget *part : {_dlgTransparent, onLoseFocus, _dlgTransparentAlways, _dlgTransparency})
		gtk_box_pack_start(GTK_BOX(transparencyBox), part, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(transparencyFrame), transparencyBox);
	auto transparencyChanged = +[](GtkWidget *, gpointer data) {
		auto *window = static_cast<EditorWindow *>(data);
		window->applyFindTransparency(gtk_window_is_active(GTK_WINDOW(window->_findWindow)));
	};
	g_signal_connect(_dlgTransparent, "toggled", G_CALLBACK(transparencyChanged), this);
	g_signal_connect(_dlgTransparentAlways, "toggled", G_CALLBACK(transparencyChanged), this);
	g_signal_connect(_dlgTransparency, "value-changed", G_CALLBACK(transparencyChanged), this);
	GtkWidget *frames = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(frames), modeFrame, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(frames), transparencyFrame, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(grid), frames, 0, 5, 3, 1);

	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_pack_start(GTK_BOX(body), buttons, FALSE, FALSE, 0);
	const std::pair<FindOperation, const char *> definitions[] = {
		{FindOperation::Next, "Localizar _próximo"},
		{FindOperation::Count, "_Contar"},
		{FindOperation::FindAllCurrent, "Localizar tudo no documento _atual"},
		{FindOperation::FindAllOpen, "Localizar tudo nos documentos ab_ertos"},
		{FindOperation::Replace, "_Substituir"},
		{FindOperation::ReplaceAll, "Substituir _tudo"},
		{FindOperation::ReplaceAllOpen, "Substituir tudo nos documentos abertos"},
		{FindOperation::MarkAll, "_Marcar tudo"},
		{FindOperation::ClearMarks, "_Limpar todas as marcas"},
		{FindOperation::CopyMarked, "Copiar texto _marcado"}};
	for (const auto &[operation, label] : definitions)
	{
		GtkWidget *button = gtk_button_new_with_mnemonic(label);
		g_object_set_data(G_OBJECT(button), "npp-find-operation", GINT_TO_POINTER(static_cast<int>(operation)));
		g_signal_connect(button, "clicked", G_CALLBACK(findButtonClicked), this);
		gtk_box_pack_start(GTK_BOX(buttons), button, FALSE, FALSE, 0);
		_dlgButtons.emplace_back(operation, button);
	}
	GtkWidget *inFiles = gtk_button_new_with_mnemonic("Localizar em ar_quivos…");
	g_signal_connect_swapped(inFiles, "clicked", G_CALLBACK(+[](gpointer data) { static_cast<EditorWindow *>(data)->openFindInFilesFromDialog(); }), this);
	gtk_box_pack_start(GTK_BOX(buttons), inFiles, FALSE, FALSE, 0);
	GtkWidget *close = gtk_button_new_with_mnemonic("_Fechar");
	g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_hide), _findWindow);
	gtk_box_pack_end(GTK_BOX(buttons), close, FALSE, FALSE, 0);

	_dlgStatus = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(_dlgStatus), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(_dlgStatus), PANGO_ELLIPSIZE_END);
	gtk_box_pack_end(GTK_BOX(outer), _dlgStatus, FALSE, FALSE, 0);
}

void EditorWindow::applyFindTransparency(bool focused)
{
	if (!_findWindow)
		return;
	double opacity = 1.0;
	if (active(_dlgTransparent) && (!focused || active(_dlgTransparentAlways)))
		opacity = gtk_range_get_value(GTK_RANGE(_dlgTransparency)) / 100.0;
	gtk_widget_set_opacity(_findWindow, opacity);
}

void EditorWindow::updateFindDialogTab(int tab)
{
	_findDialogTab = tab;
	const bool replace = tab == 1;
	const bool mark = tab == 2;
	gtk_widget_set_visible(_dlgReplaceLabel, replace);
	gtk_widget_set_visible(_dlgReplace, replace);
	gtk_widget_set_visible(_dlgSwap, replace);
	gtk_widget_set_visible(_dlgBookmarkLine, mark);
	gtk_widget_set_visible(_dlgPurge, mark);
	gtk_widget_set_visible(_dlgBackward, !mark);
	for (const auto &[operation, button] : _dlgButtons)
	{
		bool visible = false;
		switch (operation)
		{
			case FindOperation::Next: visible = !mark; break;
			case FindOperation::Count: case FindOperation::FindAllCurrent: case FindOperation::FindAllOpen: visible = tab == 0; break;
			case FindOperation::Replace: case FindOperation::ReplaceAll: case FindOperation::ReplaceAllOpen: visible = replace; break;
			default: visible = mark; break;
		}
		gtk_widget_set_visible(button, visible);
	}
	const char *titles[] = {"Localizar", "Substituir", "Marcar"};
	gtk_window_set_title(GTK_WINDOW(_findWindow), titles[std::clamp(tab, 0, 2)]);
	gtk_label_set_text(GTK_LABEL(_dlgStatus), "");
}

void EditorWindow::showFindDialog(int tab)
{
	buildFindDialog();
	EditorDocument *document = currentDocument();
	if (document)
	{
		const sptr_t start = document->send(SCI_GETSELECTIONSTART);
		const sptr_t end = document->send(SCI_GETSELECTIONEND);
		bool multiLine = false;
		if (end > start && end - start < 2000)
		{
			std::string selection(static_cast<std::size_t>(end - start) + 1, '\0');
			Sci_TextRangeFull range{{start, end}, selection.data()};
			document->send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&range));
			selection.resize(static_cast<std::size_t>(end - start));
			multiLine = selection.find_first_of("\r\n") != std::string::npos;
			if (!multiLine)
				setComboText(_dlgFind, selection);
		}
		// A selection of several lines is searched within, as in the Windows version.
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(_dlgInSelection), multiLine && tab != 0);
	}
	if (!_selfTesting)
		gtk_widget_show_all(_findWindow);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(_findTabs), tab);
	updateFindDialogTab(tab);
	if (_selfTesting)
		return;
	gtk_window_present(GTK_WINDOW(_findWindow));
	GtkWidget *entry = gtk_bin_get_child(GTK_BIN(tab == 1 && !comboText(_dlgFind).empty() ? _dlgReplace : _dlgFind));
	gtk_widget_grab_focus(entry);
	gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

void EditorWindow::openFindInFilesFromDialog()
{
	const std::string needle = _findWindow ? comboText(_dlgFind) : std::string();
	perform(Action::FindInFiles);
	if (_findText && !needle.empty())
		gtk_entry_set_text(GTK_ENTRY(_findText), needle.c_str());
	if (_findReplacement && _findWindow && !comboText(_dlgReplace).empty())
		gtk_entry_set_text(GTK_ENTRY(_findReplacement), comboText(_dlgReplace).c_str());
}

void EditorWindow::setFindStatus(const std::string &message)
{
	if (_dlgStatus)
		gtk_label_set_text(GTK_LABEL(_dlgStatus), message.c_str());
	setStatusMessage(message);
}

bool EditorWindow::runFindOperation(FindOperation operation, int direction)
{
	buildFindDialog();
	EditorDocument *document = currentDocument();
	if (!document)
		return false;
	std::string needle = comboText(_dlgFind);
	std::string replacement = comboText(_dlgReplace);
	const bool needsText = operation != FindOperation::ClearMarks && operation != FindOperation::CopyMarked;
	if (needsText && needle.empty())
	{
		setFindStatus("Digite o texto a localizar.");
		return false;
	}
	if (needsText)
		rememberFindText(_dlgFind, needle);
	const bool replacing = operation == FindOperation::Replace || operation == FindOperation::ReplaceAll || operation == FindOperation::ReplaceAllOpen;
	if (replacing)
		rememberFindText(_dlgReplace, replacement);
	SearchOptions options;
	options.matchCase = active(_dlgMatchCase);
	options.wholeWord = active(_dlgWholeWord);
	options.wrapAround = active(_dlgWrap);
	options.regularExpression = active(_dlgRegex);
	if (active(_dlgExtended))
	{
		std::string expanded;
		std::string expandedReplacement;
		if (!expandExtended(needle, expanded) || !expandExtended(replacement, expandedReplacement))
		{
			setFindStatus("Sequência com \\ inválida no modo Estendido.");
			return false;
		}
		needle = expanded;
		replacement = expandedReplacement;
	}
	if (options.regularExpression && needsText && !validateRegularExpression(needle))
	{
		setFindStatus("Expressão regular inválida.");
		return false;
	}
	_dialogSearchActive = true;
	if (needle.find_first_of("\r\n") == std::string::npos && needle.find('\0') == std::string::npos)
		gtk_entry_set_text(GTK_ENTRY(_searchEntry), needle.c_str());
	const int flags = searchFlags(options);
	sptr_t start = document->send(SCI_GETSELECTIONSTART);
	sptr_t end = document->send(SCI_GETSELECTIONEND);
	const bool inSelection = active(_dlgInSelection) && end > start;
	if (!inSelection)
	{
		start = 0;
		end = document->send(SCI_GETLENGTH);
	}
	const std::string scope = inSelection ? " na seleção." : ".";
	bool success = true;
	switch (operation)
	{
		case FindOperation::Next:
		{
			const bool backwards = direction < 0 ? active(_dlgBackward) : direction == 1;
			const SearchResult result = find(*document, needle, options, backwards);
			success = result.found;
			setFindStatus(!result.found ? "Não encontrado: “" + comboText(_dlgFind) + "”."
				: result.wrapped ? "Busca continuou do outro extremo do documento." : "Ocorrência encontrada.");
			break;
		}
		case FindOperation::Count:
		{
			const std::size_t count = countRange(*document, needle, flags, start, end);
			setFindStatus("Contar: " + std::to_string(count) + " ocorrência(s)" + scope);
			success = count > 0;
			break;
		}
		case FindOperation::FindAllCurrent:
		case FindOperation::FindAllOpen:
		{
			const auto summary = findInOpenDocuments(needle, options, operation == FindOperation::FindAllCurrent);
			setFindStatus(std::to_string(summary.totalMatches()) + " ocorrência(s) em " + std::to_string(summary.files.size()) + " documento(s).");
			success = summary.totalMatches() > 0;
			break;
		}
		case FindOperation::Replace:
		{
			const std::size_t replaced = replaceCurrent(*document, needle, replacement, options);
			const SearchResult next = find(*document, needle, options, direction < 0 ? active(_dlgBackward) : direction == 1);
			setFindStatus(replaced ? (next.found ? "1 ocorrência substituída; a próxima está selecionada." : "1 ocorrência substituída; não há mais.")
				: next.found ? "Ocorrência selecionada; clique de novo para substituir." : "Não encontrado: “" + comboText(_dlgFind) + "”.");
			success = replaced > 0 || next.found;
			break;
		}
		case FindOperation::ReplaceAll:
		{
			sptr_t rangeEnd = end;
			const std::size_t count = replaceRange(*document, needle, replacement, flags, options.regularExpression, start, rangeEnd);
			if (inSelection)
				document->send(SCI_SETSEL, start, rangeEnd);
			setFindStatus("Substituir tudo: " + std::to_string(count) + " ocorrência(s) substituída(s)" + scope);
			success = count > 0;
			break;
		}
		case FindOperation::ReplaceAllOpen:
		{
			if (!_selfTesting)
			{
				GtkWidget *question = gtk_message_dialog_new(GTK_WINDOW(_findWindow), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
					"Substituir em todos os documentos abertos?");
				gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(question), "Pode ser desfeito em cada documento com Ctrl+Z.");
				gtk_dialog_add_buttons(GTK_DIALOG(question), "_Cancelar", GTK_RESPONSE_CANCEL, "_Substituir", GTK_RESPONSE_ACCEPT, nullptr);
				const bool accepted = gtk_dialog_run(GTK_DIALOG(question)) == GTK_RESPONSE_ACCEPT;
				gtk_widget_destroy(question);
				if (!accepted)
					return false;
			}
			const auto summary = replaceInOpenDocuments(needle, replacement, options);
			std::size_t total = 0;
			for (const auto &file : summary.files)
				total += file.replaced;
			setFindStatus("Substituir tudo: " + std::to_string(total) + " ocorrência(s) em " + std::to_string(summary.files.size()) + " documento(s).");
			success = total > 0;
			break;
		}
		case FindOperation::MarkAll:
		{
			if (active(_dlgPurge))
			{
				document->send(SCI_SETINDICATORCURRENT, FindMarkIndicator);
				document->send(SCI_INDICATORCLEARRANGE, 0, document->send(SCI_GETLENGTH));
				if (active(_dlgBookmarkLine))
					document->send(SCI_MARKERDELETEALL, BookmarkMarker);
			}
			const std::size_t count = markRange(*document, needle, flags, start, end, active(_dlgBookmarkLine));
			setFindStatus("Marcar: " + std::to_string(count) + " ocorrência(s) marcada(s)" + scope);
			success = count > 0;
			break;
		}
		case FindOperation::ClearMarks:
			document->send(SCI_SETINDICATORCURRENT, FindMarkIndicator);
			document->send(SCI_INDICATORCLEARRANGE, 0, document->send(SCI_GETLENGTH));
			setFindStatus("Marcas removidas.");
			break;
		case FindOperation::CopyMarked:
		{
			const std::string text = styledText(*document, FindMarkStyle);
			if (text.empty())
			{
				setFindStatus("Não há texto marcado.");
				success = false;
			}
			else
			{
				copyToClipboard(text);
				setFindStatus("Texto marcado copiado para a área de transferência.");
			}
			break;
		}
	}
	refreshDocument(document);
	return success;
}
}
