// Desktop integration that the Windows version has outside its menus:
// a single editor window, warning when another program changes an open file,
// and the right-click menus of the text and of the tabs.
#include "Integration.h"

#include "EditorWindow.h"

#include <functional>

#include <sys/stat.h>

#include <gio/gio.h>

#include "Scintilla.h"

namespace NppLinux
{
namespace
{
void contextActionActivated(GtkMenuItem *item, gpointer data)
{
	const auto action = static_cast<Action>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "npp-action")));
	static_cast<EditorWindow *>(data)->perform(action);
}

GtkWidget *contextItem(GtkWidget *menu, const char *label, EditorWindow *window, Action action)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	g_object_set_data(G_OBJECT(item), "npp-action", GINT_TO_POINTER(static_cast<int>(action)));
	g_signal_connect(item, "activate", G_CALLBACK(contextActionActivated), window);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

void contextSeparator(GtkWidget *menu)
{
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

GtkWidget *contextSubmenu(GtkWidget *menu, const char *label)
{
	GtkWidget *sub = gtk_menu_new();
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return sub;
}

struct EventTarget
{
	EditorWindow *window;
	EditorDocument *document;
};

gboolean editorButtonPressed(GtkWidget *, GdkEventButton *event, gpointer data)
{
	if (event->type != GDK_BUTTON_PRESS || event->button != 3)
		return FALSE;
	static_cast<EditorWindow *>(data)->popupContextMenu(false, reinterpret_cast<GdkEvent *>(event));
	return TRUE;
}

gboolean tabButtonPressed(GtkWidget *, GdkEventButton *event, gpointer data)
{
	auto *target = static_cast<EventTarget *>(data);
	if (event->type != GDK_BUTTON_PRESS || (event->button != 2 && event->button != 3))
		return FALSE;
	target->window->tabClicked(target->document, static_cast<int>(event->button), reinterpret_cast<GdkEvent *>(event));
	return TRUE;
}

bool ask(GtkWindow *parent, const std::string &question, const std::string &detail, const char *accept, const char *refuse)
{
	GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", question.c_str());
	if (!detail.empty())
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail.c_str());
	gtk_dialog_add_buttons(GTK_DIALOG(dialog), refuse, GTK_RESPONSE_NO, accept, GTK_RESPONSE_YES, nullptr);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
	const bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES;
	gtk_widget_destroy(dialog);
	return accepted;
}
}

std::filesystem::path instanceSocketPath()
{
	// Each configuration folder gets its own editor, so test runs with another
	// XDG_CONFIG_HOME never talk to the person's editor.
	const std::string config = g_get_user_config_dir();
	const std::string name = "notepad-plus-plus-linux-" + std::to_string(std::hash<std::string>{}(config) % 100000000) + ".sock";
	return std::filesystem::path(g_get_user_runtime_dir()) / name;
}

bool sendToRunningInstance(const std::filesystem::path &socket, const std::vector<std::filesystem::path> &files)
{
	std::error_code error;
	if (!std::filesystem::exists(socket, error))
		return false;
	GSocketClient *client = g_socket_client_new();
	GSocketAddress *address = g_unix_socket_address_new(socket.c_str());
	GSocketConnection *connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), nullptr, nullptr);
	g_object_unref(address);
	g_object_unref(client);
	if (!connection)
		return false;
	std::string message = "NPP-LINUX-OPEN 1\n";
	for (const auto &file : files)
		message += std::filesystem::absolute(file).lexically_normal().string() + "\n";
	GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
	const bool sent = g_output_stream_write_all(output, message.data(), message.size(), nullptr, nullptr, nullptr);
	g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
	g_object_unref(connection);
	return sent;
}

bool EditorWindow::startInstanceServer(const std::filesystem::path &socket)
{
	stopInstanceServer();
	std::error_code error;
	// The caller found no editor answering, so a file left here is stale.
	std::filesystem::remove(socket, error);
	GSocketService *service = g_socket_service_new();
	GSocketAddress *address = g_unix_socket_address_new(socket.c_str());
	const bool listening = g_socket_listener_add_address(G_SOCKET_LISTENER(service), address, G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_DEFAULT, nullptr, nullptr, nullptr);
	g_object_unref(address);
	if (!listening)
	{
		g_object_unref(service);
		return false;
	}
	chmod(socket.c_str(), S_IRUSR | S_IWUSR);
	g_signal_connect(service, "incoming", G_CALLBACK(+[](GSocketService *, GSocketConnection *connection, GObject *, gpointer data) -> gboolean {
		auto *window = static_cast<EditorWindow *>(data);
		GDataInputStream *input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
		std::vector<std::string> lines;
		gchar *line = nullptr;
		while (lines.size() < 10000 && (line = g_data_input_stream_read_line(input, nullptr, nullptr, nullptr)) != nullptr)
		{
			lines.emplace_back(line);
			g_free(line);
		}
		g_object_unref(input);
		if (lines.empty() || lines.front() != "NPP-LINUX-OPEN 1")
			return TRUE;
		std::vector<std::filesystem::path> files;
		for (std::size_t i = 1; i < lines.size(); ++i)
		{
			if (!lines[i].empty() && lines[i].front() == '/')
				files.emplace_back(lines[i]);
		}
		window->openFromAnotherStart(files);
		return TRUE;
	}), this);
	g_socket_service_start(service);
	_instanceService = service;
	_instanceSocket = socket;
	return true;
}

void EditorWindow::stopInstanceServer()
{
	if (!_instanceService)
		return;
	g_socket_service_stop(_instanceService);
	g_socket_listener_close(G_SOCKET_LISTENER(_instanceService));
	g_object_unref(_instanceService);
	_instanceService = nullptr;
	std::error_code error;
	std::filesystem::remove(_instanceSocket, error);
	_instanceSocket.clear();
}

void EditorWindow::openFromAnotherStart(const std::vector<std::filesystem::path> &files)
{
	for (const auto &file : files)
		openPath(file);
	if (!_selfTesting)
		gtk_window_present(GTK_WINDOW(_window));
}

void EditorWindow::buildContextMenus()
{
	_editorContextMenu = gtk_menu_new();
	GtkWidget *menu = _editorContextMenu;
	contextItem(menu, "Recor_tar", this, Action::Cut);
	contextItem(menu, "_Copiar", this, Action::Copy);
	contextItem(menu, "Co_lar", this, Action::Paste);
	contextItem(menu, "_Excluir", this, Action::DeleteSelection);
	contextItem(menu, "Selecionar _tudo", this, Action::SelectAll);
	contextSeparator(menu);
	contextItem(menu, "Iniciar/terminar _seleção", this, Action::BeginEndSelect);
	contextSeparator(menu);
	GtkWidget *style = contextSubmenu(menu, "_Destacar todas as ocorrências");
	const char *styles[] = {"Usando o 1º estilo", "Usando o 2º estilo", "Usando o 3º estilo", "Usando o 4º estilo", "Usando o 5º estilo"};
	for (int i = 0; i < 5; ++i)
		contextItem(style, styles[i], this, static_cast<Action>(static_cast<int>(Action::StyleToken1) + i));
	contextItem(menu, "Limpar todos os destaques", this, Action::ClearAllStyles);
	contextSeparator(menu);
	contextItem(menu, "_MAIÚSCULAS", this, Action::UpperCase);
	contextItem(menu, "m_inúsculas", this, Action::LowerCase);
	contextSeparator(menu);
	contextItem(menu, "_Abrir arquivo selecionado", this, Action::OpenSelectedFile);
	contextItem(menu, "Pesquisar na i_nternet", this, Action::SearchOnInternet);
	contextSeparator(menu);
	contextItem(menu, "Alternar comentário de lin_ha", this, Action::ToggleComment);
	contextItem(menu, "Comentário em _bloco", this, Action::BlockComment);
	contextItem(menu, "Remover comentário em bloco", this, Action::BlockUncomment);
	contextSeparator(menu);
	contextItem(menu, "_Ocultar linhas", this, Action::HideLines);
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), _window, nullptr);

	_tabContextMenu = gtk_menu_new();
	menu = _tabContextMenu;
	contextItem(menu, "_Fechar", this, Action::CloseFile);
	contextItem(menu, "Fechar todos _exceto este", this, Action::CloseAllButActive);
	contextItem(menu, "Fechar todos à es_querda", this, Action::CloseAllToLeft);
	contextItem(menu, "Fechar todos à _direita", this, Action::CloseAllToRight);
	contextItem(menu, "Fechar todos os _inalterados", this, Action::CloseAllUnchanged);
	contextSeparator(menu);
	contextItem(menu, "_Salvar", this, Action::SaveFile);
	contextItem(menu, "Salvar _como…", this, Action::SaveFileAs);
	contextItem(menu, "_Renomear…", this, Action::RenameFile);
	contextItem(menu, "Mover para a _lixeira", this, Action::MoveToTrash);
	contextItem(menu, "Recarre_gar do disco", this, Action::ReloadFromDisk);
	contextItem(menu, "Alternar somente leitu_ra", this, Action::ToggleReadOnly);
	contextSeparator(menu);
	contextItem(menu, "Copiar caminho com_pleto", this, Action::CopyFullPath);
	contextItem(menu, "Copiar _nome do arquivo", this, Action::CopyFileName);
	contextItem(menu, "Copiar _pasta", this, Action::CopyDirectoryPath);
	contextSeparator(menu);
	contextItem(menu, "Abrir pasta no gerenciador de _arquivos", this, Action::OpenContainingFolder);
	contextItem(menu, "Abrir _terminal nesta pasta", this, Action::OpenContainingTerminal);
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), _window, nullptr);
}

void EditorWindow::attachDocumentEvents(EditorDocument *document, GtkWidget *tabArea)
{
	// The editor shows our own right-click menu instead of Scintilla's short one.
	document->send(SCI_USEPOPUP, SC_POPUP_NEVER);
	g_signal_connect(document->widget(), "button-press-event", G_CALLBACK(editorButtonPressed), this);
	auto *target = new EventTarget{this, document};
	g_signal_connect_data(tabArea, "button-press-event", G_CALLBACK(tabButtonPressed), target,
		+[](gpointer data, GClosure *) { delete static_cast<EventTarget *>(data); }, static_cast<GConnectFlags>(0));
}

void EditorWindow::popupContextMenu(bool tab, GdkEvent *event)
{
	GtkWidget *menu = tab ? _tabContextMenu : _editorContextMenu;
	if (menu && !_selfTesting)
		gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
}

void EditorWindow::tabClicked(EditorDocument *document, int button, GdkEvent *event)
{
	const int page = gtk_notebook_page_num(GTK_NOTEBOOK(_notebook), document->widget());
	if (page < 0)
		return;
	if (button == 2)
	{
		// Middle click closes the tab, as in the Windows version.
		closeDocument(static_cast<std::size_t>(page));
		return;
	}
	setCurrentPage(page);
	popupContextMenu(true, event);
}

bool EditorWindow::checkExternalChanges(bool force)
{
	if (!_settings.fileAutoDetect || _checkingExternal || (_selfTesting && !force))
		return false;
	_checkingExternal = true;
	bool changed = false;
	for (std::size_t i = 0; i < _documents.size(); ++i)
	{
		EditorDocument *document = _documents[i].get();
		if (document->path.empty() || document->monitoring)
			continue;
		std::error_code error;
		if (!std::filesystem::is_regular_file(document->path, error))
		{
			if (document->missingNotified)
				continue;
			document->missingNotified = true;
			changed = true;
			// The text stays open and is marked unsaved, so it can be saved again.
			markModified(*document);
			refreshDocument(document);
			const std::string message = "“" + document->displayName + "” foi apagado ou movido por outro programa. "
				"O texto continua aberto aqui; salve-o para não perdê-lo.";
			setStatusMessage(message);
			if (!_selfTesting)
				showMessage(message);
			continue;
		}
		document->missingNotified = false;
		const auto size = std::filesystem::file_size(document->path, error);
		const auto time = std::filesystem::last_write_time(document->path, error);
		if (error || (size == document->monitoredSize && time == document->monitoredTime))
			continue;
		document->monitoredSize = size;
		document->monitoredTime = time;
		changed = true;
		const bool modified = document->send(SCI_GETMODIFY) != 0;
		bool reload = false;
		if (_settings.autoReloadSilently && !modified)
			reload = true;
		else if (_selfTesting)
			reload = !modified;
		else
		{
			setCurrentPage(static_cast<int>(i));
			reload = ask(GTK_WINDOW(_window), "“" + document->displayName + "” foi alterado por outro programa.",
				modified ? "Recarregar o arquivo do disco? As alterações feitas aqui e ainda não salvas serão perdidas."
					: "Recarregar o arquivo do disco?", "_Recarregar", "_Manter o que está aqui");
		}
		if (reload)
			reloadFromDisk(*document, false);
	}
	_checkingExternal = false;
	return changed;
}
}
