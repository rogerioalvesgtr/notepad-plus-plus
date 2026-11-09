/*
 * Notepad++ Linux — native plugin interface (API version 1).
 *
 * A plugin is a shared library (.so) built for Linux. It is placed in
 *   ~/.local/share/notepad-plus-plus/plugins/            (name.so or name/name.so)
 * or in the system folder installed with the editor
 *   <prefix>/lib/notepad-plus-plus-linux/plugins/
 *
 * New plugins stay disabled until the user enables them in
 * Plugins -> Gerenciar plugins. Plugins run inside the editor process with the
 * user's permissions; only install plugins you trust.
 *
 * Windows plugin DLLs cannot be loaded: they must be rebuilt against this API.
 *
 * Minimal plugin:
 *
 *   #include "npp-linux-plugin.h"
 *   static void hello(const NppLinuxHost *host) { host->showMessage(host->context, "Olá!"); }
 *   static const NppLinuxPluginCommand commands[] = {{"Dizer olá", hello}};
 *   static const NppLinuxPluginInfo info = {NPP_LINUX_PLUGIN_API_VERSION, "Olá", "1.0",
 *       "Exemplo", commands, 1};
 *   NPP_LINUX_PLUGIN_EXPORT const NppLinuxPluginInfo *npp_linux_plugin_info(void) { return &info; }
 *
 * Build: cc -shared -fPIC -I<path>/include hello.c -o hello.so
 */
#ifndef NPP_LINUX_PLUGIN_H
#define NPP_LINUX_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPP_LINUX_PLUGIN_API_VERSION 1
#define NPP_LINUX_PLUGIN_EXPORT __attribute__((visibility("default")))

/* Services offered by the editor. Every function receives host->context. */
typedef struct NppLinuxHost
{
	int apiVersion;
	void *context;
	/* Sends a Scintilla message (see Scintilla.h) to the current document; returns 0 without a document. */
	intptr_t (*sendEditor)(void *context, unsigned int message, uintptr_t wParam, intptr_t lParam);
	/* Text of the current document / selection, UTF-8. Free the result with freeText. Never NULL. */
	char *(*getText)(void *context);
	char *(*getSelection)(void *context);
	/* Replaces the selection (or inserts at the caret) as one undoable step. */
	void (*replaceSelection)(void *context, const char *text);
	/* Path of the current file, or "" for an unsaved document. Free with freeText. */
	char *(*getFilePath)(void *context);
	void (*openFile)(void *context, const char *path);
	void (*newDocument)(void *context, const char *text);
	void (*showMessage)(void *context, const char *message);
	void (*setStatus)(void *context, const char *message);
	void (*freeText)(char *text);
} NppLinuxHost;

typedef void (*NppLinuxCommandFunction)(const NppLinuxHost *host);

typedef struct NppLinuxPluginCommand
{
	const char *name; /* menu label, UTF-8 */
	NppLinuxCommandFunction run;
} NppLinuxPluginCommand;

typedef struct NppLinuxPluginInfo
{
	int apiVersion; /* NPP_LINUX_PLUGIN_API_VERSION */
	const char *name;
	const char *version;
	const char *description;
	const NppLinuxPluginCommand *commands;
	int commandCount; /* 0..100 */
} NppLinuxPluginInfo;

/* Required export. */
typedef const NppLinuxPluginInfo *(*NppLinuxPluginInfoFunction)(void);
#define NPP_LINUX_PLUGIN_INFO_SYMBOL "npp_linux_plugin_info"

/* Optional exports: called once after loading and once when the editor closes. */
typedef int (*NppLinuxPluginLoadFunction)(const NppLinuxHost *host); /* return 0 to refuse loading */
typedef void (*NppLinuxPluginUnloadFunction)(void);
#define NPP_LINUX_PLUGIN_LOAD_SYMBOL "npp_linux_plugin_load"
#define NPP_LINUX_PLUGIN_UNLOAD_SYMBOL "npp_linux_plugin_unload"

#ifdef __cplusplus
}
#endif

#endif
