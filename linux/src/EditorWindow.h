#ifndef NPP_LINUX_EDITOR_WINDOW_H
#define NPP_LINUX_EDITOR_WINDOW_H

#include "BuiltinTools.h"
#include "EditorDocument.h"
#include "FileSearch.h"
#include "FunctionList.h"
#include "LineTools.h"
#include "NppStyles.h"
#include "PluginManager.h"
#include "Search.h"
#include "Settings.h"
#include "SessionStore.h"
#include "Theme.h"
#include "UserLanguage.h"
#include "Workspace.h"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace NppLinux
{
enum class Action
{
	NewFile,
	OpenFile,
	SaveFile,
	SaveFileAs,
	CloseFile,
	Undo,
	Redo,
	Cut,
	Copy,
	Paste,
	DeleteSelection,
	SelectAll,
	Find,
	Replace,
	GoToLine,
	ToggleWordWrap,
	ToggleWhitespace,
	ToggleEndOfLine,
	ToggleLineNumbers,
	ToggleDarkTheme,
	ShowPreferences,
	ConvertToWindowsEol,
	ConvertToUnixEol,
	ZoomIn,
	ZoomOut,
	ZoomReset,
	ShowAbout,
	ShowRoadmap,
	StartMacro,
	StopMacro,
	PlayMacro,
	RunMacroAgain,
	ClearMacro,
	BackupNow,
	FindInFiles,
	NextResult,
	PreviousResult,
	ToggleResults,
	ToggleWorkspace,
	ToggleFunctionList,
	ToggleDocumentMap,
	OpenWorkspaceFolder,
	OpenWorkspaceFile,
	SaveWorkspaceFile,
	StyleConfigurator,
	ManageUserLanguages,
	ImportUserLanguage,
	ManagePlugins,
	OpenPluginFolder,
	DuplicateLine,
	DeleteLine,
	MoveLineUp,
	MoveLineDown,
	SortLinesAscending,
	SortLinesDescending,
	RemoveDuplicateLines,
	TrimTrailingWhitespace,
	UpperCase,
	LowerCase,
	ToggleComment,
	// Windows menu parity (implemented in MenuCommands.cpp).
	OpenContainingFolder, OpenContainingTerminal, OpenFolderAsWorkspace, OpenInDefaultViewer, ReloadFromDisk,
	SaveCopyAs, SaveAll, RenameFile, CloseAll, CloseAllButActive, CloseAllToLeft, CloseAllToRight,
	CloseAllUnchanged, MoveToTrash, LoadSessionFile, SaveSessionFile, Print, PrintNow,
	InsertDateShort, InsertDateLong, InsertDateCustom,
	CopyFullPath, CopyFileName, CopyDirectoryPath, CopyAllNames, CopyAllPaths,
	IndentIncrease, IndentDecrease,
	ProperCase, ProperCaseBlend, SentenceCase, SentenceCaseBlend, InvertCase, RandomCase,
	RemoveConsecutiveDuplicates, SplitLines, JoinLines, RemoveEmptyLines, RemoveBlankLines,
	InsertLineAbove, InsertLineBelow, ReverseLines, RandomizeLines,
	SortLexicalAsc, SortIgnoreCaseAsc, SortLocaleAsc, SortIntegerAsc, SortDecimalCommaAsc, SortDecimalDotAsc, SortLengthAsc,
	SortLexicalDesc, SortIgnoreCaseDesc, SortLocaleDesc, SortIntegerDesc, SortDecimalCommaDesc, SortDecimalDotDesc, SortLengthDesc,
	LineComment, LineUncomment, BlockComment, BlockUncomment,
	WordCompletion, PathCompletion, ConvertToMacEol,
	TrimLeading, TrimBoth, EolToSpace, TrimBothEolToSpace, TabsToSpaces, SpacesToTabsAll, SpacesToTabsLeading,
	OpenSelectedFile, OpenSelectedFolder, SearchOnInternet,
	SelectAllMatches, SelectAllMatchesCase, SelectAllMatchesWord, SelectAllMatchesCaseWord,
	SelectNextMatch, SelectNextMatchCase, SelectNextMatchWord, SelectNextMatchCaseWord,
	UndoLatestMultiSelect, SkipMultiSelect, ColumnModeInfo, ColumnEditor,
	ToggleReadOnly, ReadOnlyAll, ClearReadOnlyAll,
	FindNext, FindPrevious, SelectFindNext, SelectFindPrevious, GoToMatchingBrace, SelectBetweenBraces,
	StyleToken1, StyleToken2, StyleToken3, StyleToken4, StyleToken5,
	ClearStyle1, ClearStyle2, ClearStyle3, ClearStyle4, ClearStyle5, ClearAllStyles, JumpStyleUp, JumpStyleDown,
	ToggleBookmark, NextBookmark, PreviousBookmark, ClearBookmarks, CutBookmarkedLines, CopyBookmarkedLines,
	RemoveBookmarkedLines, RemoveUnbookmarkedLines, InverseBookmarks, NextChange, PreviousChange,
	AlwaysOnTop, FullScreen, DistractionFree, ShowAllCharacters, ToggleIndentGuides, ToggleWrapSymbol,
	NextTab, PreviousTab, FirstTab, LastTab, MoveTabStart, MoveTabEnd, MoveTabForward, MoveTabBackward,
	Tab1, Tab2, Tab3, Tab4, Tab5, Tab6, Tab7, Tab8, Tab9,
	FoldAll, UnfoldAll, FoldCurrent, UnfoldCurrent,
	FoldLevel1, FoldLevel2, FoldLevel3, FoldLevel4, FoldLevel5, FoldLevel6, FoldLevel7, FoldLevel8,
	UnfoldLevel1, UnfoldLevel2, UnfoldLevel3, UnfoldLevel4, UnfoldLevel5, UnfoldLevel6, UnfoldLevel7, UnfoldLevel8,
	HideLines, ShowHiddenLines, Summary, Monitoring,
	ShortcutList, ImportPlugin, OpenUserLanguageFolder,
	HashMd5Text, HashMd5Files, HashMd5Selection, HashSha1Text, HashSha1Files, HashSha1Selection,
	HashSha256Text, HashSha256Files, HashSha256Selection, HashSha512Text, HashSha512Files, HashSha512Selection,
	SaveMacroAs, RunMacroMultiple, RunCommand, OpenInBrowser,
	WindowsList, SortTabsNameAsc, SortTabsNameDesc, SortTabsPathAsc, SortTabsPathDesc,
	SortTabsTypeAsc, SortTabsTypeDesc, SortTabsSizeAsc, SortTabsSizeDesc,
	CommandLineHelp, DebugInfo,
	// Items confirmed in the installed Windows 8.9.7 menus.
	RestoreClosedFile, OpenAllRecent, ClearRecentList, BeginEndSelect, BeginEndSelectColumn, ToggleMacroRecord,
	FindVolatileNext, FindVolatilePrevious,
	StyleOneToken1, StyleOneToken2, StyleOneToken3, StyleOneToken4, StyleOneToken5,
	CopyStyled1, CopyStyled2, CopyStyled3, CopyStyled4, CopyStyled5, CopyStyledAll,
	PasteReplaceBookmarked, SortTabsTimeAsc, SortTabsTimeDesc,
	// Built-in versions of popular Windows plugins.
	CompareWith, CompareNext, ComparePrevious, CompareClear,
	Base64Encode, Base64Decode, UrlEncode, UrlDecode, AsciiToHex, HexToAscii,
	ExportHtml, ExportRtf, JsMinify, JsFormat,
	MarkDialog, IncrementalSearch,
	Quit
};


class EditorWindow
{
public:
	explicit EditorWindow(bool selfTesting = false, bool noSession = false, bool recoveryOnly = false);
	~EditorWindow();

	EditorWindow(const EditorWindow &) = delete;
	EditorWindow &operator=(const EditorWindow &) = delete;

	void show();
	void openPath(const std::filesystem::path &path);
	EditorDocument *createDocument(const std::string &text = {});
	bool saveDocument(EditorDocument &document, bool askPath = false);
	bool closeDocument(std::size_t index, bool askToSave = true, bool discardBackup = true);
	void perform(Action action);
	void applySettings();
	void applyTheme(EditorDocument &document);
	void chooseLanguage(const Language &language);
	void refreshDocument(EditorDocument *document = nullptr);
	void reorderDocument(gint from, gint to);
	bool showFindResults(const std::string &operation);
	SearchOptions searchOptions() const;
	EditorDocument *currentDocument() const;
	std::size_t currentIndex() const;
	std::size_t documentCount() const noexcept;
	friend gboolean backupTimer(gpointer data);
	bool runSelfTest(const std::filesystem::path &directory, std::string &result);
	bool runRecoverySmokeTest(const std::filesystem::path &directory, std::string &result);
	bool recordMacro(unsigned int message, uptr_t wParam, sptr_t lParam, const char *text);
	void updateMacroState();
	void moveRecordingToCurrentPage();
	bool backupTick();

	FileSearchSummary findInFiles(const std::filesystem::path &folder, const std::string &needle,
		const FileSearchOptions &options);
	FileSearchSummary replaceInFiles(const std::filesystem::path &folder, const std::string &needle,
		const std::string &replacement, const FileSearchOptions &options, bool confirm = true);
	FileSearchSummary findInOpenDocuments(const std::string &needle, const SearchOptions &options, bool onlyCurrent = false);
	FileSearchSummary replaceInOpenDocuments(const std::string &needle, const std::string &replacement,
		const SearchOptions &options);
	bool goToResult(std::size_t index);
	bool nextResult(bool backwards = false);
	void activateResultRow(GtkTreePath *path);
	void runFindInFilesDialog(int response);
	void chooseFindFolder();
	void hideResults();

	// Stage 4: navigation panels.
	void scheduleFunctionList(bool immediate = false);
	void refreshFunctionList();
	void activateFunctionRow(GtkTreePath *path);
	void syncDocumentMap();
	void documentMapJumpToLine(sptr_t line);
	void documentMapJumpToPoint(double x, double y);
	void documentMapScroll(int lines);
	bool addWorkspaceFolder(const std::filesystem::path &folder);
	void removeSelectedWorkspaceFolder();
	void rebuildWorkspace();
	void populateWorkspaceNode(GtkTreeIter *iter);
	void activateWorkspaceRow(GtkTreePath *path);
	void findInSelectedWorkspaceFolder();
	void chooseWorkspaceFolder();
	bool openWorkspaceFile(const std::filesystem::path &file);
	bool saveWorkspaceFile(const std::filesystem::path &file);
	guint _functionTimer = 0;

	// Stage 5: styles, preferences, user languages, plugins and line tools.
	void styleNeeded(EditorDocument *document, sptr_t end);
	void charAdded(EditorDocument *document, int character);
	void updateBraceMatch(EditorDocument &document);
	const Language &languageFor(const std::filesystem::path &path) const;
	const Language *languageById(const std::string &id) const;
	const UserLanguage *userLanguageFor(const Language *language) const;
	std::filesystem::path userLanguageDirectory() const;
	void reloadUserLanguages();
	std::size_t importUserLanguage(const std::filesystem::path &file, std::string &error);
	bool saveUserLanguageDefinition(UserLanguage language, std::string &error);
	bool removeUserLanguage(const std::string &name);
	void rebuildLanguageMenu();
	void rebuildPluginMenu();
	void scanPlugins(const std::vector<std::filesystem::path> &directories);
	bool enablePlugin(const std::string &id, bool enabled);
	bool runPluginCommand(std::size_t plugin, std::size_t command);
	void showMessage(const std::string &message);
	void setStatusMessage(const std::string &message);
	std::string commentPrefix(const EditorDocument &document) const;
	void restartBackupTimer();
	void showPreferences();
	void showStyleConfigurator();
	void showUserLanguageDialog();
	void showPluginManager();

	// Windows menu parity (MenuCommands.cpp).
	void performMore(Action action);
	bool quitKeepsUnsaved() const;
	bool convertEncoding(EditorDocument &document, const std::string &encoding);
	bool reopenWithEncoding(const std::string &encoding);
	void buildEncodingMenu(GtkWidget *menu);
	void updateEncodingMenu();
	void rebuildWindowMenu();
	void rebuildMacroMenu();
	bool runMacroFile(const std::filesystem::path &file, std::size_t repetitions, bool untilEnd);
	bool runMacroCommands(const std::vector<MacroCommand> &commands, std::size_t repetitions, bool untilEnd);
	void marginClicked(EditorDocument *document, int margin, sptr_t position);
	void applyMargins(EditorDocument &document);
	bool toggleBookmark(EditorDocument &document, sptr_t line);
	void monitorTick();
	void closeMultiple(Action which);
	bool openSessionFile(const std::filesystem::path &file);
	bool saveSessionFileAs(const std::filesystem::path &file);
	void printDocument(bool showDialog);
	bool styleAllOccurrences(EditorDocument &document, int style);
	std::size_t bookmarkedLineCount(EditorDocument &document);
	void foldLevel(EditorDocument &document, int level, bool fold);
	void sortTabs(Action order);
	void moveTab(std::size_t from, std::size_t to);
	void showHashDialog(int kind, bool files);
	void showColumnEditor();
	void showRunDialog();
	std::string documentSummary(EditorDocument &document);
	std::string shortcutListText() const;
	std::vector<std::string> completionWords(EditorDocument &document) const;
	bool renameDocument(EditorDocument &document, const std::string &name);
	void goToPage(int page);
	struct ColumnInsert
	{
		bool numbers = false;
		std::string text;
		long long initial = 1;
		long long increment = 1;
		long long repeat = 1;
		int padding = 0; // 0 none, 1 zeros, 2 spaces
		int base = 0;    // 0 decimal, 1 hex, 2 octal, 3 binary
	};
	void insertColumn(EditorDocument &document, const ColumnInsert &insert);
	void showShortcutList();
	void showWindowsList();
	void showTextDialog(const std::string &title, const std::string &text);
	void copyToClipboard(const std::string &text);
	std::string currentWordOrSelection(EditorDocument &document) const;
	std::map<std::string, std::string> runVariables(EditorDocument *document) const;
	void markModified(EditorDocument &document);
	bool reloadFromDisk(EditorDocument &document, bool ask);
	GtkWidget *_encodingMenu = nullptr;
	std::vector<std::pair<GtkWidget *, std::string>> _encodingItems;
	GtkWidget *_windowMenu = nullptr;
	GtkWidget *_macroMenu = nullptr;
	GtkWidget *_menuBar = nullptr;
	GtkWidget *_toolbar = nullptr;
	GtkWidget *_statusBar = nullptr;
	GtkWidget *_readOnlyCheck = nullptr;
	GtkWidget *_alwaysOnTopCheck = nullptr;
	GtkWidget *_fullScreenCheck = nullptr;
	GtkWidget *_distractionFreeCheck = nullptr;
	GtkWidget *_allCharactersCheck = nullptr;
	GtkWidget *_indentGuidesCheck = nullptr;
	GtkWidget *_wrapSymbolCheck = nullptr;
	GtkWidget *_monitoringCheck = nullptr;
	bool _fullScreen = false;
	bool _distractionFree = false;
	guint _monitorTimer = 0;
	std::vector<MacroCommand> _lastMacroForRun;
	std::vector<std::string> _closedFiles;
	std::string styledText(EditorDocument &document, int style) const;
	std::size_t compareDocuments(EditorDocument &left, EditorDocument &right, bool report);
	void clearComparison();
	// Side-by-side comparison window, like ComparePlus on Windows and the split
	// view of GitHub (CompareView.cpp).
	void showCompareView(EditorDocument &left, EditorDocument &right, const std::vector<DiffLine> &diff);
	void fillComparePane(EditorDocument &pane, const EditorDocument &source, const std::vector<std::string> &lines,
		const std::vector<WordChanges> &words, bool leftSide);
	void compareJump(bool forward);
	void compareScrolled(EditorDocument &from);
	void compareMarginClicked(bool leftSide, sptr_t position);
	void refreshCompareView();
	void openCompareReport();
	void compareDocumentClosed(const EditorDocument *document);
	void updateCompareStatus();
	void drawCompareOverview(cairo_t *cr, int width, int height);
	void compareOverviewClicked(double y, int height);
	GtkWidget *_compareWindow = nullptr;
	GtkWidget *_compareLeftTitle = nullptr;
	GtkWidget *_compareRightTitle = nullptr;
	GtkWidget *_compareStatus = nullptr;
	GtkWidget *_compareOverview = nullptr;
	GtkWidget *_compareRefreshButton = nullptr;
	GtkWidget *_compareReportButton = nullptr;
	std::unique_ptr<EditorDocument> _compareLeft;
	std::unique_ptr<EditorDocument> _compareRight;
	EditorDocument *_compareLeftSource = nullptr;
	EditorDocument *_compareRightSource = nullptr;
	std::vector<CompareRow> _compareRows;
	std::vector<std::size_t> _compareBlocks; // first row of each difference
	std::size_t _compareTextHash = 0;
	sptr_t _compareCaretRow = 0;
	bool _compareSyncing = false;
	bool _compareIgnoreSpaces = false;
	bool _compareIgnoreCase = false;
	std::string documentHtml(EditorDocument &document);
	std::string documentRtf(EditorDocument &document);
	// Find / Replace / Mark dialog (FindDialog.cpp).
	enum class FindOperation { Next, Count, FindAllCurrent, FindAllOpen, Replace, ReplaceAll, ReplaceAllOpen, MarkAll, ClearMarks, CopyMarked };
	void showFindDialog(int tab);
	void buildFindDialog();
	void updateFindDialogTab(int tab);
	// direction: -1 uses the dialog's "backward" option, 0 forward, 1 backward.
	bool runFindOperation(FindOperation operation, int direction = -1);
	void rememberFindText(GtkWidget *combo, const std::string &text);
	void applyFindTransparency(bool focused);
	void openFindInFilesFromDialog();
	void setFindStatus(const std::string &message);
	GtkWidget *_findWindow = nullptr;
	GtkWidget *_findTabs = nullptr;
	GtkWidget *_dlgFind = nullptr;
	GtkWidget *_dlgReplace = nullptr;
	GtkWidget *_dlgReplaceLabel = nullptr;
	GtkWidget *_dlgSwap = nullptr;
	GtkWidget *_dlgBackward = nullptr;
	GtkWidget *_dlgWholeWord = nullptr;
	GtkWidget *_dlgMatchCase = nullptr;
	GtkWidget *_dlgWrap = nullptr;
	GtkWidget *_dlgInSelection = nullptr;
	GtkWidget *_dlgNormal = nullptr;
	GtkWidget *_dlgExtended = nullptr;
	GtkWidget *_dlgRegex = nullptr;
	GtkWidget *_dlgBookmarkLine = nullptr;
	GtkWidget *_dlgPurge = nullptr;
	GtkWidget *_dlgStatus = nullptr;
	GtkWidget *_dlgTransparent = nullptr;
	GtkWidget *_dlgTransparency = nullptr;
	GtkWidget *_dlgTransparentAlways = nullptr;
	std::vector<std::pair<FindOperation, GtkWidget *>> _dlgButtons;
	bool _dialogSearchActive = false;
	int _findDialogTab = 0;
	// Desktop integration (Integration.cpp).
	bool startInstanceServer(const std::filesystem::path &socket);
	void stopInstanceServer();
	void openFromAnotherStart(const std::vector<std::filesystem::path> &files);
	void buildContextMenus();
	void attachDocumentEvents(EditorDocument *document, GtkWidget *tabArea);
	void popupContextMenu(bool tab, GdkEvent *event);
	void tabClicked(EditorDocument *document, int button, GdkEvent *event);
	bool checkExternalChanges(bool force);
	GSocketService *_instanceService = nullptr;
	std::filesystem::path _instanceSocket;
	GtkWidget *_editorContextMenu = nullptr;
	GtkWidget *_tabContextMenu = nullptr;
	bool _checkingExternal = false;
	NppLanguageSpec nppSpecFor(const EditorDocument &document) const;
	bool applyTextTool(EditorDocument &document, const std::function<bool(const std::string &, std::string &)> &tool, const std::string &failure);

private:
	struct ResultLocation
	{
		std::filesystem::path path;
		const EditorDocument *document = nullptr;
		sptr_t start = 0;
		sptr_t end = 0;
		std::size_t line = 0;
		std::string rowPath;
	};

	void buildResultsPanel();
	void buildWorkspacePanel();
	void buildSidebar();
	void applyPanels();
	// source: the document whose colours to use (the document map shows another document's text).
	void applyStyles(EditorDocument &view, const Language &language, const EditorDocument *source = nullptr);
	void applyViewOptions(EditorDocument &document);
	std::string selectedWorkspacePath(bool directoryOnly) const;
	void showFindInFiles();
	void showResults(const std::string &title, const FileSearchSummary &summary, bool replacing);
	OpenDocumentLookup openDocumentLookup() const;
	bool confirmFileReplacement(const std::filesystem::path &folder, const FileSearchOptions &options,
		const std::string &needle, const std::string &replacement);

	void buildUi();
	void buildMenu();
	void buildToolbar();
	void buildSearchBar();
	void setCurrentPage(int page);
	void updateTitle();
	void updateStatus();
	void updateTab(EditorDocument &document);
	void updateLanguageMenu();
	void showFind(bool replace);
	void showError(const std::string &message);
	bool confirmDiscard(EditorDocument &document);
	bool saveAllModified();
	void addRecentFile(const std::filesystem::path &path);
	void rebuildRecentMenu();
	void saveSettings();
	void loadSession();
	void saveSession();
	bool backupDocuments();
	bool restoreEntry(const SessionEntry &entry);
	bool playMacro(std::size_t repetitions = 1);
	void stopMacroRecording();

	GtkWidget *_window = nullptr;
	GtkWidget *_notebook = nullptr;
	GtkWidget *_positionLabel = nullptr;
	GtkWidget *_languageLabel = nullptr;
	GtkWidget *_encodingLabel = nullptr;
	GtkWidget *_eolLabel = nullptr;
	GtkWidget *_messageLabel = nullptr;
	GtkWidget *_searchBar = nullptr;
	GtkWidget *_searchEntry = nullptr;
	GtkWidget *_replaceEntry = nullptr;
	GtkWidget *_replaceLabel = nullptr;
	GtkWidget *_replaceButton = nullptr;
	GtkWidget *_replaceAllButton = nullptr;
	GtkWidget *_matchCase = nullptr;
	GtkWidget *_wholeWord = nullptr;
	GtkWidget *_regularExpression = nullptr;
	GtkWidget *_wrapAround = nullptr;
	GtkWidget *_recentMenu = nullptr;
	GtkWidget *_languageMenu = nullptr;
	GtkWidget *_wrapCheck = nullptr;
	GtkWidget *_whitespaceCheck = nullptr;
	GtkWidget *_eolCheck = nullptr;
	GtkWidget *_lineNumbersCheck = nullptr;
	GtkWidget *_darkCheck = nullptr;
	GtkWidget *_recordMenuItem = nullptr;
	GtkWidget *_stopMacroMenuItem = nullptr;
	GtkWidget *_playMacroMenuItem = nullptr;
	GtkWidget *_repeatMacroMenuItem = nullptr;
	GtkWidget *_clearMacroMenuItem = nullptr;
	GtkWidget *_paned = nullptr;
	GtkWidget *_resultsPanel = nullptr;
	GtkWidget *_resultsHeader = nullptr;
	GtkWidget *_resultsView = nullptr;
	GtkTreeStore *_resultsStore = nullptr;
	GtkWidget *_findDialog = nullptr;
	GtkWidget *_findDialogStatus = nullptr;
	GtkWidget *_findText = nullptr;
	GtkWidget *_findReplacement = nullptr;
	GtkWidget *_findFilters = nullptr;
	GtkWidget *_findFolder = nullptr;
	GtkWidget *_findRecursive = nullptr;
	GtkWidget *_findHidden = nullptr;
	GtkWidget *_findMatchCase = nullptr;
	GtkWidget *_findWholeWord = nullptr;
	GtkWidget *_findRegex = nullptr;
	std::unique_ptr<FileSearcher> _fileSearcher;
	std::vector<ResultLocation> _results;
	std::size_t _currentResult = static_cast<std::size_t>(-1);
	bool _resultsPositioned = false;
	GtkWidget *_outerPaned = nullptr;
	GtkWidget *_innerPaned = nullptr;
	GtkWidget *_workspacePanel = nullptr;
	GtkWidget *_workspaceView = nullptr;
	GtkTreeStore *_workspaceStore = nullptr;
	GtkWidget *_sidebar = nullptr;
	GtkWidget *_functionPanel = nullptr;
	GtkWidget *_functionView = nullptr;
	GtkWidget *_functionFilter = nullptr;
	GtkWidget *_functionSort = nullptr;
	GtkWidget *_functionStatus = nullptr;
	GtkTreeStore *_functionStore = nullptr;
	std::vector<FunctionEntry> _functions;
	const EditorDocument *_functionDocument = nullptr;
	GtkWidget *_mapPanel = nullptr;
	std::unique_ptr<EditorDocument> _mapView;
	sptr_t _mapDocumentPointer = 0;
	const Language *_mapLanguage = nullptr;
	GtkWidget *_workspaceCheck = nullptr;
	GtkWidget *_functionListCheck = nullptr;
	GtkWidget *_documentMapCheck = nullptr;
	Theme _theme;
	std::vector<UserLanguage> _userLanguages;
	std::deque<Language> _userLanguageEntries;
	PluginManager _plugins;
	NppLinuxHost _pluginHost{};
	GtkWidget *_pluginMenu = nullptr;
	GtkWidget *_mapWidget = nullptr;
	bool _selfTesting = false;
	bool _noSession = false;
	bool _sessionWritable = false;
	Settings _settings;
	SettingsStore _settingsStore;
	SessionStore _sessionStore;
	std::vector<MacroCommand> _macro;
	bool _recordingMacro = false;
	bool _playingMacro = false;
	guint _backupTimer = 0;
	std::vector<std::unique_ptr<EditorDocument>> _documents;
	bool _updating = false;
	int _untitledCounter = 0;
	std::filesystem::path _selfTestDirectory;
};
}

#endif
