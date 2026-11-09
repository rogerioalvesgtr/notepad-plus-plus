// Side-by-side comparison window: the two documents next to each other with
// aligned lines, removed text in red on the left and added text in green on
// the right, the changed words in a stronger shade, like the split view of
// GitHub and the ComparePlus plugin of the Windows version.
#include "EditorWindow.h"

#include <algorithm>
#include <functional>

#include "Scintilla.h"
#include "ScintillaWidget.h"

namespace NppLinux
{
namespace
{
constexpr int RemovedMarker = 20;
constexpr int AddedMarker = 21;
constexpr int FillerMarker = 22;
constexpr int RemovedWordIndicator = 28;
constexpr int AddedWordIndicator = 29;
enum MarginStyle { MarginNormal, MarginRemoved, MarginAdded, MarginFiller, MarginStyleCount };

struct Tint
{
	int red, green, blue;
};
constexpr Tint Red{248, 81, 73};
constexpr Tint Green{46, 160, 67};
constexpr Tint Grey{128, 128, 128};

// Colour (Scintilla order: red in the low byte) mixed with a tint.
int blend(int back, Tint tint, double amount)
{
	auto mix = [amount](int base, int target) { return static_cast<int>(base + (target - base) * amount + 0.5); };
	return mix(back & 0xff, tint.red) | (mix((back >> 8) & 0xff, tint.green) << 8) | (mix((back >> 16) & 0xff, tint.blue) << 16);
}

bool isDark(int colour)
{
	return (colour & 0xff) * 299 + ((colour >> 8) & 0xff) * 587 + ((colour >> 16) & 0xff) * 114 < 128000;
}

std::string escapeMarkup(const std::string &text)
{
	gchar *escaped = g_markup_escape_text(text.c_str(), -1);
	std::string result = escaped ? escaped : "";
	g_free(escaped);
	return result;
}

void paneNotified(GtkWidget *widget, gint, SCNotification *notification, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	const bool leftSide = g_object_get_data(G_OBJECT(widget), "npp-compare-left") != nullptr;
	EditorDocument *pane = leftSide ? window->_compareLeft.get() : window->_compareRight.get();
	if (!pane)
		return;
	switch (notification->nmhdr.code)
	{
		case SCN_UPDATEUI:
			if (notification->updated & (SC_UPDATE_V_SCROLL | SC_UPDATE_H_SCROLL))
				window->compareScrolled(*pane);
			if (notification->updated & SC_UPDATE_SELECTION)
			{
				window->_compareCaretRow = pane->send(SCI_LINEFROMPOSITION, pane->send(SCI_GETCURRENTPOS));
				window->updateCompareStatus();
			}
			break;
		case SCN_ZOOM:
		{
			EditorDocument *other = leftSide ? window->_compareRight.get() : window->_compareLeft.get();
			if (other && other->send(SCI_GETZOOM) != pane->send(SCI_GETZOOM))
				other->send(SCI_SETZOOM, static_cast<uptr_t>(pane->send(SCI_GETZOOM)));
			break;
		}
		case SCN_MARGINCLICK:
			window->compareMarginClicked(leftSide, notification->position);
			break;
		default:
			break;
	}
}

gboolean compareKeyPressed(GtkWidget *, GdkEventKey *event, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	const guint state = event->state & gtk_accelerator_get_default_mod_mask();
	const bool alt = state == GDK_MOD1_MASK;
	switch (event->keyval)
	{
		case GDK_KEY_Page_Down: case GDK_KEY_Down:
			if (!alt)
				return FALSE;
			window->compareJump(true);
			return TRUE;
		case GDK_KEY_Page_Up: case GDK_KEY_Up:
			if (!alt)
				return FALSE;
			window->compareJump(false);
			return TRUE;
		case GDK_KEY_F5:
			window->refreshCompareView();
			return TRUE;
		case GDK_KEY_Escape:
			gtk_widget_destroy(window->_compareWindow);
			return TRUE;
		default:
			return FALSE;
	}
}

gboolean compareFocused(GtkWidget *, GdkEvent *, gpointer data)
{
	// Coming back after editing one of the documents: compare again.
	auto *window = static_cast<EditorWindow *>(data);
	if (window->_compareLeftSource && window->_compareRightSource)
	{
		const std::size_t hash = std::hash<std::string>{}(window->_compareLeftSource->text())
			^ (std::hash<std::string>{}(window->_compareRightSource->text()) << 1);
		if (hash != window->_compareTextHash)
			window->refreshCompareView();
	}
	return FALSE;
}

void compareDestroyed(GtkWidget *, gpointer data)
{
	auto *window = static_cast<EditorWindow *>(data);
	window->_compareWindow = nullptr;
	window->_compareLeft.reset();
	window->_compareRight.reset();
	window->_compareLeftSource = nullptr;
	window->_compareRightSource = nullptr;
	window->_compareRows.clear();
	window->_compareBlocks.clear();
}

gboolean overviewDraw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	static_cast<EditorWindow *>(data)->drawCompareOverview(cr, gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
	return TRUE;
}

gboolean overviewPressed(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	static_cast<EditorWindow *>(data)->compareOverviewClicked(event->y, gtk_widget_get_allocated_height(widget));
	return TRUE;
}

gboolean overviewMoved(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
	if (event->state & GDK_BUTTON1_MASK)
		static_cast<EditorWindow *>(data)->compareOverviewClicked(event->y, gtk_widget_get_allocated_height(widget));
	return TRUE;
}

GtkWidget *toolButton(const char *label, const char *tooltip, GCallback callback, gpointer data)
{
	GtkWidget *button = gtk_button_new_with_mnemonic(label);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
	g_signal_connect(button, "clicked", callback, data);
	return button;
}
}

void EditorWindow::showCompareView(EditorDocument &left, EditorDocument &right, const std::vector<DiffLine> &diff)
{
	const bool refreshing = _compareWindow && _compareLeftSource == &left && _compareRightSource == &right;
	_compareLeftSource = &left;
	_compareRightSource = &right;
	const std::string leftText = left.text();
	const std::string rightText = right.text();
	_compareTextHash = std::hash<std::string>{}(leftText) ^ (std::hash<std::string>{}(rightText) << 1);
	_compareRows = alignDiff(diff);
	_compareBlocks.clear();
	std::size_t removed = 0;
	std::size_t added = 0;
	for (std::size_t row = 0; row < _compareRows.size(); ++row)
	{
		const char kind = _compareRows[row].kind;
		if (kind != ' ' && (row == 0 || _compareRows[row - 1].kind == ' '))
			_compareBlocks.push_back(row);
		removed += kind == '~' || kind == '-';
		added += kind == '~' || kind == '+';
	}

	if (!_compareWindow)
	{
		_compareWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		gtk_window_set_transient_for(GTK_WINDOW(_compareWindow), GTK_WINDOW(_window));
		gtk_window_set_destroy_with_parent(GTK_WINDOW(_compareWindow), TRUE);
		gtk_window_set_icon_name(GTK_WINDOW(_compareWindow), "notepad-plus-plus-linux");
		int width = 1100;
		int height = 720;
		gtk_window_get_size(GTK_WINDOW(_window), &width, &height);
		gtk_window_set_default_size(GTK_WINDOW(_compareWindow), std::max(width, 900), std::max(height, 600));
		g_signal_connect(_compareWindow, "destroy", G_CALLBACK(compareDestroyed), this);
		g_signal_connect(_compareWindow, "key-press-event", G_CALLBACK(compareKeyPressed), this);
		g_signal_connect(_compareWindow, "focus-in-event", G_CALLBACK(compareFocused), this);

		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);
		gtk_box_pack_start(GTK_BOX(toolbar), toolButton("◀ _Anterior", "Diferença anterior (Alt+PgUp)",
			G_CALLBACK(+[](GtkWidget *, gpointer data) { static_cast<EditorWindow *>(data)->compareJump(false); }), this), FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(toolbar), toolButton("_Próxima ▶", "Próxima diferença (Alt+PgDn)",
			G_CALLBACK(+[](GtkWidget *, gpointer data) { static_cast<EditorWindow *>(data)->compareJump(true); }), this), FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
		_compareRefreshButton = toolButton("⟳ A_tualizar", "Comparar de novo depois de editar os documentos (F5)",
			G_CALLBACK(+[](GtkWidget *, gpointer data) { static_cast<EditorWindow *>(data)->refreshCompareView(); }), this);
		gtk_box_pack_start(GTK_BOX(toolbar), _compareRefreshButton, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(toolbar), toolButton("⇄ _Trocar lados", "Colocar o documento da direita à esquerda",
			G_CALLBACK(+[](GtkWidget *, gpointer data) {
				auto *window = static_cast<EditorWindow *>(data);
				if (window->_compareLeftSource && window->_compareRightSource)
					window->compareDocuments(*window->_compareRightSource, *window->_compareLeftSource, true);
			}), this), FALSE, FALSE, 0);
		auto option = [&](const char *label, const char *tooltip, bool active, GCallback callback) {
			GtkWidget *check = gtk_check_button_new_with_mnemonic(label);
			gtk_widget_set_tooltip_text(check, tooltip);
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), active);
			g_signal_connect(check, "toggled", callback, this);
			gtk_box_pack_start(GTK_BOX(toolbar), check, FALSE, FALSE, 6);
		};
		option("Ignorar _espaços", "Linhas que só diferem em espaços e tabulações contam como iguais", _compareIgnoreSpaces,
			G_CALLBACK(+[](GtkToggleButton *button, gpointer data) {
				auto *window = static_cast<EditorWindow *>(data);
				window->_compareIgnoreSpaces = gtk_toggle_button_get_active(button);
				window->refreshCompareView();
			}));
		option("Ignorar _maiúsculas", "Linhas que só diferem em maiúsculas e minúsculas contam como iguais", _compareIgnoreCase,
			G_CALLBACK(+[](GtkToggleButton *button, gpointer data) {
				auto *window = static_cast<EditorWindow *>(data);
				window->_compareIgnoreCase = gtk_toggle_button_get_active(button);
				window->refreshCompareView();
			}));
		_compareReportButton = toolButton("_Relatório diff", "Abrir as diferenças no formato diff em uma nova aba",
			G_CALLBACK(+[](GtkWidget *, gpointer data) { static_cast<EditorWindow *>(data)->openCompareReport(); }), this);
		gtk_box_pack_end(GTK_BOX(toolbar), _compareReportButton, FALSE, FALSE, 0);
		_compareStatus = gtk_label_new("");
		gtk_widget_set_tooltip_text(_compareStatus, "Clique no número de uma linha para ir até ela no documento original.");
		gtk_label_set_ellipsize(GTK_LABEL(_compareStatus), PANGO_ELLIPSIZE_END);
		gtk_box_pack_end(GTK_BOX(toolbar), _compareStatus, TRUE, TRUE, 8);
		gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, FALSE, 0);

		GtkWidget *panes = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
		auto side = [&](bool leftSide) {
			GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
			GtkWidget *title = gtk_label_new("");
			gtk_label_set_xalign(GTK_LABEL(title), 0.0);
			gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
			gtk_widget_set_margin_start(title, 8);
			gtk_widget_set_margin_end(title, 8);
			gtk_widget_set_margin_top(title, 4);
			gtk_widget_set_margin_bottom(title, 4);
			gtk_box_pack_start(GTK_BOX(column), title, FALSE, FALSE, 0);
			GtkWidget *widget = scintilla_new();
			gtk_widget_set_size_request(widget, 200, 100);
			if (leftSide)
				g_object_set_data(G_OBJECT(widget), "npp-compare-left", GINT_TO_POINTER(1));
			g_signal_connect(widget, SCINTILLA_NOTIFY, G_CALLBACK(paneNotified), this);
			gtk_box_pack_start(GTK_BOX(column), widget, TRUE, TRUE, 0);
			(leftSide ? _compareLeftTitle : _compareRightTitle) = title;
			(leftSide ? _compareLeft : _compareRight) = std::make_unique<EditorDocument>(widget);
			return column;
		};
		gtk_paned_pack1(GTK_PANED(panes), side(true), TRUE, FALSE);
		gtk_paned_pack2(GTK_PANED(panes), side(false), TRUE, FALSE);
		int paneWidth = 1100;
		gtk_window_get_default_size(GTK_WINDOW(_compareWindow), &paneWidth, nullptr);
		gtk_paned_set_position(GTK_PANED(panes), std::max(200, (paneWidth - 18) / 2));

		GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_box_pack_start(GTK_BOX(body), panes, TRUE, TRUE, 0);
		_compareOverview = gtk_drawing_area_new();
		gtk_widget_set_size_request(_compareOverview, 18, -1);
		gtk_widget_set_tooltip_text(_compareOverview, "Visão geral das diferenças: clique para ir até elas");
		gtk_widget_add_events(_compareOverview, GDK_BUTTON_PRESS_MASK | GDK_BUTTON1_MOTION_MASK);
		g_signal_connect(_compareOverview, "draw", G_CALLBACK(overviewDraw), this);
		g_signal_connect(_compareOverview, "button-press-event", G_CALLBACK(overviewPressed), this);
		g_signal_connect(_compareOverview, "motion-notify-event", G_CALLBACK(overviewMoved), this);
		gtk_box_pack_start(GTK_BOX(body), _compareOverview, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(box), body, TRUE, TRUE, 0);
		gtk_container_add(GTK_CONTAINER(_compareWindow), box);
	}

	const sptr_t firstLine = refreshing ? _compareLeft->send(SCI_GETFIRSTVISIBLELINE) : 0;
	const sptr_t xOffset = refreshing ? _compareLeft->send(SCI_GETXOFFSET) : 0;
	std::vector<WordChanges> words(_compareRows.size());
	const auto leftLines = splitLines(leftText);
	const auto rightLines = splitLines(rightText);
	for (std::size_t row = 0; row < _compareRows.size(); ++row)
	{
		const CompareRow &line = _compareRows[row];
		if (line.kind == '~')
			words[row] = wordDiff(leftLines[static_cast<std::size_t>(line.left)], rightLines[static_cast<std::size_t>(line.right)]);
	}
	_compareSyncing = true;
	fillComparePane(*_compareLeft, left, leftLines, words, true);
	fillComparePane(*_compareRight, right, rightLines, words, false);
	_compareSyncing = false;

	auto title = [&](const EditorDocument &document, std::size_t count, bool leftSide) {
		std::string markup = "<b>" + escapeMarkup(document.displayName) + "</b>";
		if (count)
			markup += leftSide ? "   <span foreground=\"#f85149\"><b>−" + std::to_string(count) + "</b></span>"
				: "   <span foreground=\"#3fb950\"><b>+" + std::to_string(count) + "</b></span>";
		return markup;
	};
	gtk_label_set_markup(GTK_LABEL(_compareLeftTitle), title(left, removed, true).c_str());
	gtk_label_set_markup(GTK_LABEL(_compareRightTitle), title(right, added, false).c_str());
	gtk_widget_set_tooltip_text(_compareLeftTitle, left.path.empty() ? left.displayName.c_str() : left.path.c_str());
	gtk_widget_set_tooltip_text(_compareRightTitle, right.path.empty() ? right.displayName.c_str() : right.path.c_str());
	const std::string windowTitle = "Comparar: " + left.displayName + " ↔ " + right.displayName;
	gtk_window_set_title(GTK_WINDOW(_compareWindow), windowTitle.c_str());
	gtk_widget_set_sensitive(_compareRefreshButton, TRUE);
	gtk_widget_set_sensitive(_compareReportButton, !_compareBlocks.empty());

	if (refreshing)
	{
		_compareLeft->send(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(firstLine));
		_compareLeft->send(SCI_SETXOFFSET, static_cast<uptr_t>(xOffset));
		compareScrolled(*_compareLeft);
		updateCompareStatus();
	}
	else
	{
		_compareCaretRow = -1;
		if (!_compareBlocks.empty())
			compareJump(true);
		else
			updateCompareStatus();
	}
	gtk_widget_queue_draw(_compareOverview);
	if (!_selfTesting)
	{
		gtk_widget_show_all(_compareWindow);
		gtk_window_present(GTK_WINDOW(_compareWindow));
	}
}

void EditorWindow::fillComparePane(EditorDocument &pane, const EditorDocument &source, const std::vector<std::string> &lines,
	const std::vector<WordChanges> &words, bool leftSide)
{
	pane.path = source.path;
	pane.displayName = source.displayName;
	pane.language = source.language;
	pane.send(SCI_SETREADONLY, 0);
	std::string text;
	std::vector<sptr_t> offsets(_compareRows.size(), 0);
	for (std::size_t row = 0; row < _compareRows.size(); ++row)
	{
		if (row)
			text += '\n';
		offsets[row] = static_cast<sptr_t>(text.size());
		const long index = leftSide ? _compareRows[row].left : _compareRows[row].right;
		if (index >= 0 && static_cast<std::size_t>(index) < lines.size())
			text += lines[static_cast<std::size_t>(index)];
	}
	pane.send(SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED);
	pane.setText(text);
	applyTheme(pane);
	pane.send(SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED);
	pane.send(SCI_SETWRAPMODE, SC_WRAP_NONE);
	pane.send(SCI_SETCARETLINEVISIBLE, 0);
	pane.send(SCI_SETENDATLASTLINE, 0);
	for (int margin = 1; margin < 5; ++margin)
		pane.send(SCI_SETMARGINWIDTHN, static_cast<uptr_t>(margin), 0);

	const int back = static_cast<int>(pane.send(SCI_STYLEGETBACK, STYLE_DEFAULT));
	const bool dark = isDark(back);
	const Tint tint = leftSide ? Red : Green;
	pane.send(SCI_MARKERDEFINE, leftSide ? RemovedMarker : AddedMarker, SC_MARK_BACKGROUND);
	pane.send(SCI_MARKERSETBACK, leftSide ? RemovedMarker : AddedMarker, blend(back, tint, dark ? 0.18 : 0.13));
	pane.send(SCI_MARKERDEFINE, FillerMarker, SC_MARK_BACKGROUND);
	pane.send(SCI_MARKERSETBACK, FillerMarker, blend(back, Grey, dark ? 0.16 : 0.09));
	const int indicator = leftSide ? RemovedWordIndicator : AddedWordIndicator;
	pane.send(SCI_INDICSETSTYLE, static_cast<uptr_t>(indicator), INDIC_STRAIGHTBOX);
	pane.send(SCI_INDICSETFORE, static_cast<uptr_t>(indicator), blend(back, tint, dark ? 0.45 : 0.38));
	pane.send(SCI_INDICSETALPHA, static_cast<uptr_t>(indicator), 255);
	pane.send(SCI_INDICSETOUTLINEALPHA, static_cast<uptr_t>(indicator), 255);
	pane.send(SCI_INDICSETUNDER, static_cast<uptr_t>(indicator), 1);

	// Line numbers of the original document; empty filler rows have none.
	pane.send(SCI_RELEASEALLEXTENDEDSTYLES);
	const sptr_t base = pane.send(SCI_ALLOCATEEXTENDEDSTYLES, MarginStyleCount);
	pane.send(SCI_MARGINSETSTYLEOFFSET, static_cast<uptr_t>(base));
	const int numberFore = static_cast<int>(pane.send(SCI_STYLEGETFORE, STYLE_LINENUMBER));
	const int numberBack = static_cast<int>(pane.send(SCI_STYLEGETBACK, STYLE_LINENUMBER));
	const int marginColours[MarginStyleCount][2] = {
		{numberFore, numberBack},
		{numberFore, blend(back, Red, dark ? 0.32 : 0.25)},
		{numberFore, blend(back, Green, dark ? 0.32 : 0.25)},
		{numberFore, blend(back, Grey, dark ? 0.16 : 0.09)}};
	const sptr_t size = pane.send(SCI_STYLEGETSIZE, STYLE_DEFAULT);
	for (int i = 0; i < MarginStyleCount; ++i)
	{
		const auto style = static_cast<uptr_t>(base + i);
		pane.send(SCI_STYLESETFONT, style, reinterpret_cast<sptr_t>("Monospace"));
		pane.send(SCI_STYLESETSIZE, style, std::max<sptr_t>(6, size - 1));
		pane.send(SCI_STYLESETFORE, style, marginColours[i][0]);
		pane.send(SCI_STYLESETBACK, style, marginColours[i][1]);
	}
	pane.send(SCI_MARGINTEXTCLEARALL);
	pane.send(SCI_MARKERDELETEALL, static_cast<uptr_t>(-1));
	pane.send(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(indicator));
	pane.send(SCI_INDICATORCLEARRANGE, 0, pane.send(SCI_GETLENGTH));
	for (std::size_t row = 0; row < _compareRows.size(); ++row)
	{
		const CompareRow &line = _compareRows[row];
		const long index = leftSide ? line.left : line.right;
		const auto lineNumber = static_cast<uptr_t>(row);
		if (index < 0)
		{
			pane.send(SCI_MARKERADD, lineNumber, FillerMarker);
			pane.sendString(SCI_MARGINSETTEXT, lineNumber, " ");
			pane.send(SCI_MARGINSETSTYLE, lineNumber, MarginFiller);
			continue;
		}
		const bool changed = line.kind != ' ';
		std::string label = std::to_string(index + 1) + (changed ? (leftSide ? " -" : " +") : "  ");
		pane.sendString(SCI_MARGINSETTEXT, lineNumber, label);
		pane.send(SCI_MARGINSETSTYLE, lineNumber, changed ? (leftSide ? MarginRemoved : MarginAdded) : MarginNormal);
		if (!changed)
			continue;
		pane.send(SCI_MARKERADD, lineNumber, leftSide ? RemovedMarker : AddedMarker);
		for (const auto &[start, length] : leftSide ? words[row].left : words[row].right)
			pane.send(SCI_INDICATORFILLRANGE, static_cast<uptr_t>(offsets[row] + static_cast<sptr_t>(start)), static_cast<sptr_t>(length));
	}
	const std::string widest = std::to_string(std::max<std::size_t>(lines.size(), 99)) + " +";
	pane.send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_RTEXT);
	pane.send(SCI_SETMARGINWIDTHN, 0, pane.send(SCI_TEXTWIDTH, static_cast<uptr_t>(base), reinterpret_cast<sptr_t>(widest.c_str())) + 12);
	pane.send(SCI_SETMARGINSENSITIVEN, 0, 1);
	pane.send(SCI_SETMARGINCURSORN, 0, SC_CURSORARROW);
	pane.send(SCI_SETREADONLY, 1);
}

void EditorWindow::compareScrolled(EditorDocument &from)
{
	if (_compareSyncing || !_compareLeft || !_compareRight)
		return;
	EditorDocument &other = &from == _compareLeft.get() ? *_compareRight : *_compareLeft;
	_compareSyncing = true;
	if (other.send(SCI_GETFIRSTVISIBLELINE) != from.send(SCI_GETFIRSTVISIBLELINE))
		other.send(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(from.send(SCI_GETFIRSTVISIBLELINE)));
	if (other.send(SCI_GETXOFFSET) != from.send(SCI_GETXOFFSET))
		other.send(SCI_SETXOFFSET, static_cast<uptr_t>(from.send(SCI_GETXOFFSET)));
	_compareSyncing = false;
	if (_compareOverview)
		gtk_widget_queue_draw(_compareOverview);
}

void EditorWindow::compareJump(bool forward)
{
	if (!_compareLeft || !_compareRight)
		return;
	if (_compareBlocks.empty())
	{
		updateCompareStatus();
		return;
	}
	const auto caret = _compareCaretRow;
	std::size_t target = forward ? _compareBlocks.front() : _compareBlocks.back();
	if (forward)
	{
		for (const std::size_t block : _compareBlocks)
		{
			if (static_cast<sptr_t>(block) > caret)
			{
				target = block;
				break;
			}
		}
	}
	else
	{
		for (auto it = _compareBlocks.rbegin(); it != _compareBlocks.rend(); ++it)
		{
			if (static_cast<sptr_t>(*it) < caret)
			{
				target = *it;
				break;
			}
		}
	}
	_compareSyncing = true;
	for (EditorDocument *pane : {_compareLeft.get(), _compareRight.get()})
	{
		pane->send(SCI_GOTOLINE, target);
		pane->send(SCI_SETFIRSTVISIBLELINE, target > 3 ? target - 3 : 0);
	}
	_compareSyncing = false;
	_compareCaretRow = static_cast<sptr_t>(target);
	updateCompareStatus();
	if (_compareOverview)
		gtk_widget_queue_draw(_compareOverview);
}

void EditorWindow::updateCompareStatus()
{
	if (!_compareStatus)
		return;
	std::string status;
	if (!_compareLeftSource || !_compareRightSource)
		status = "Um dos documentos foi fechado.";
	else if (_compareBlocks.empty())
		status = "Os documentos são iguais.";
	else
	{
		std::size_t current = 0;
		for (std::size_t i = 0; i < _compareBlocks.size(); ++i)
		{
			if (static_cast<sptr_t>(_compareBlocks[i]) <= _compareCaretRow)
				current = i + 1;
		}
		status = (current ? "Diferença " + std::to_string(current) + " de " : "")
			+ std::to_string(_compareBlocks.size()) + (current ? "" : " diferença(s)");
	}
	gtk_label_set_text(GTK_LABEL(_compareStatus), status.c_str());
}

void EditorWindow::compareMarginClicked(bool leftSide, sptr_t position)
{
	EditorDocument *pane = leftSide ? _compareLeft.get() : _compareRight.get();
	EditorDocument *source = leftSide ? _compareLeftSource : _compareRightSource;
	if (!pane || !source || _compareRows.empty())
		return;
	auto row = static_cast<std::size_t>(pane->send(SCI_LINEFROMPOSITION, position));
	long line = -1;
	for (std::size_t r = std::min(row, _compareRows.size() - 1); r < _compareRows.size(); --r)
	{
		line = leftSide ? _compareRows[r].left : _compareRows[r].right;
		if (line >= 0)
			break;
	}
	for (std::size_t i = 0; i < _documents.size(); ++i)
	{
		if (_documents[i].get() != source)
			continue;
		setCurrentPage(static_cast<int>(i));
		const sptr_t target = std::max<long>(line, 0);
		source->send(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(target));
		source->send(SCI_GOTOLINE, static_cast<uptr_t>(target));
		gtk_window_present(GTK_WINDOW(_window));
		gtk_widget_grab_focus(source->widget());
		return;
	}
}

void EditorWindow::refreshCompareView()
{
	if (_compareLeftSource && _compareRightSource)
		compareDocuments(*_compareLeftSource, *_compareRightSource, true);
}

void EditorWindow::openCompareReport()
{
	if (!_compareLeftSource || !_compareRightSource)
		return;
	EditorDocument &left = *_compareLeftSource;
	EditorDocument &right = *_compareRightSource;
	const auto leftLines = splitLines(left.text());
	const auto rightLines = splitLines(right.text());
	std::vector<std::string> leftKeys;
	std::vector<std::string> rightKeys;
	for (const auto &line : leftLines)
		leftKeys.push_back(compareKey(line, _compareIgnoreSpaces, _compareIgnoreCase));
	for (const auto &line : rightLines)
		rightKeys.push_back(compareKey(line, _compareIgnoreSpaces, _compareIgnoreCase));
	auto diff = diffLines(leftKeys, rightKeys);
	for (auto &line : diff)
		line.text = line.kind == '+' ? rightLines[line.right] : leftLines[line.left];
	const std::string leftName = left.path.empty() ? left.displayName : left.path.string();
	const std::string rightName = right.path.empty() ? right.displayName : right.path.string();
	EditorDocument *summary = createDocument(unifiedDiff(diff, leftName, rightName));
	summary->displayName = "Comparação: " + left.displayName + " ↔ " + right.displayName;
	if (const Language *language = languageById("diff"))
	{
		summary->language = language;
		applyTheme(*summary);
	}
	refreshDocument(summary);
	if (!_selfTesting)
		gtk_window_present(GTK_WINDOW(_window));
}

void EditorWindow::compareDocumentClosed(const EditorDocument *document)
{
	if (!_compareWindow || (document != _compareLeftSource && document != _compareRightSource))
		return;
	_compareLeftSource = nullptr;
	_compareRightSource = nullptr;
	gtk_widget_set_sensitive(_compareRefreshButton, FALSE);
	gtk_widget_set_sensitive(_compareReportButton, FALSE);
	updateCompareStatus();
}

void EditorWindow::drawCompareOverview(cairo_t *cr, int width, int height)
{
	const bool dark = _compareLeft && isDark(static_cast<int>(_compareLeft->send(SCI_STYLEGETBACK, STYLE_DEFAULT)));
	cairo_set_source_rgb(cr, dark ? 0.16 : 0.95, dark ? 0.16 : 0.95, dark ? 0.17 : 0.96);
	cairo_paint(cr);
	const std::size_t rows = _compareRows.size();
	if (!rows || height <= 0 || !_compareLeft)
		return;
	const double rowHeight = static_cast<double>(height) / static_cast<double>(rows);
	const double half = width / 2.0;
	for (std::size_t row = 0; row < rows; ++row)
	{
		const char kind = _compareRows[row].kind;
		if (kind == ' ')
			continue;
		const double y = row * rowHeight;
		const double h = std::max(2.0, rowHeight);
		if (kind == '~' || kind == '-')
		{
			cairo_set_source_rgb(cr, 0.86, 0.24, 0.24);
			cairo_rectangle(cr, 2, y, half - 3, h);
			cairo_fill(cr);
		}
		if (kind == '~' || kind == '+')
		{
			cairo_set_source_rgb(cr, 0.20, 0.66, 0.30);
			cairo_rectangle(cr, half + 1, y, half - 3, h);
			cairo_fill(cr);
		}
	}
	// Part of the documents on screen.
	const double first = static_cast<double>(_compareLeft->send(SCI_GETFIRSTVISIBLELINE));
	const double visible = static_cast<double>(_compareLeft->send(SCI_LINESONSCREEN));
	cairo_set_source_rgba(cr, dark ? 1 : 0, dark ? 1 : 0, dark ? 1 : 0, 0.18);
	cairo_rectangle(cr, 0, first * rowHeight, width, std::max(4.0, visible * rowHeight));
	cairo_fill(cr);
}

void EditorWindow::compareOverviewClicked(double y, int height)
{
	if (!_compareLeft || _compareRows.empty() || height <= 0)
		return;
	const auto row = static_cast<sptr_t>(y / height * static_cast<double>(_compareRows.size()));
	const sptr_t visible = _compareLeft->send(SCI_LINESONSCREEN);
	_compareLeft->send(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(std::max<sptr_t>(0, row - visible / 2)));
	compareScrolled(*_compareLeft);
}
}
