/*
 * Example plugin for Notepad++ Linux: small text tools.
 * Build: make -C linux plugins   (produces linux/build/plugins/text-tools/text-tools.so)
 */
#define _POSIX_C_SOURCE 200809L
#include "npp-linux-plugin.h"

/* Scintilla message numbers (scintilla/include/Scintilla.h). That header is C++-only
   in this repository, so a C plugin defines the few messages it uses. */
#define SCI_SELECTALL 2013
#define SCI_GETSELECTIONEMPTY 2650

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int selectWholeDocumentIfEmpty(const NppLinuxHost *host)
{
	if (host->sendEditor(host->context, SCI_GETSELECTIONEMPTY, 0, 0))
	{
		host->sendEditor(host->context, SCI_SELECTALL, 0, 0);
		return 1;
	}
	return 0;
}

static void countWords(const NppLinuxHost *host)
{
	char *text = host->getText(host->context);
	size_t words = 0, lines = text[0] ? 1 : 0, characters = 0;
	int inWord = 0;
	for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
	{
		if ((*p & 0xC0) != 0x80)
			++characters;
		if (*p == '\n' && p[1])
			++lines;
		const int space = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r';
		if (!space && !inWord)
			++words;
		inWord = !space;
	}
	host->freeText(text);
	char message[160];
	snprintf(message, sizeof message, "%zu palavras, %zu linhas e %zu caracteres.", words, lines, characters);
	host->showMessage(host->context, message);
}

/* Applies a line transformation to the selection (or to the whole document). */
static void transformLines(const NppLinuxHost *host, int reverse, int dropBlank)
{
	selectWholeDocumentIfEmpty(host);
	char *text = host->getSelection(host->context);
	size_t length = strlen(text);
	size_t count = 0;
	char **lines = malloc(sizeof(char *) * (length + 2));
	if (!lines)
	{
		host->freeText(text);
		return;
	}
	char *start = text;
	int trailingNewline = length > 0 && text[length - 1] == '\n';
	if (trailingNewline)
		text[length - 1] = '\0';
	for (char *p = text;; ++p)
	{
		if (*p == '\n' || *p == '\0')
		{
			const int end = *p == '\0';
			*p = '\0';
			lines[count++] = start;
			start = p + 1;
			if (end)
				break;
		}
	}
	char *result = malloc(length + 2);
	if (!result)
	{
		free(lines);
		host->freeText(text);
		return;
	}
	result[0] = '\0';
	size_t written = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const char *line = lines[reverse ? count - 1 - i : i];
		size_t lineLength = strlen(line);
		const int blank = strspn(line, " \t\r") == lineLength;
		if (dropBlank && blank)
			continue;
		memcpy(result + written, line, lineLength);
		written += lineLength;
		result[written++] = '\n';
	}
	if (written > 0 && !trailingNewline)
		--written;
	result[written] = '\0';
	host->replaceSelection(host->context, result);
	free(result);
	free(lines);
	host->freeText(text);
}

static void reverseLines(const NppLinuxHost *host)
{
	transformLines(host, 1, 0);
	host->setStatus(host->context, "Ordem das linhas invertida.");
}

static void removeBlankLines(const NppLinuxHost *host)
{
	transformLines(host, 0, 1);
	host->setStatus(host->context, "Linhas em branco removidas.");
}

static void insertDateTime(const NppLinuxHost *host)
{
	char buffer[64];
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);
	strftime(buffer, sizeof buffer, "%d/%m/%Y %H:%M", &local);
	host->replaceSelection(host->context, buffer);
}

static const NppLinuxPluginCommand commands[] = {
	{"Contar palavras", countWords},
	{"Inverter ordem das linhas", reverseLines},
	{"Remover linhas em branco", removeBlankLines},
	{"Inserir data e hora", insertDateTime},
};

static const NppLinuxPluginInfo info = {
	NPP_LINUX_PLUGIN_API_VERSION,
	"Ferramentas de texto",
	"1.0",
	"Plugin de exemplo: contar palavras, inverter linhas, remover linhas em branco e inserir data e hora.",
	commands,
	(int)(sizeof commands / sizeof commands[0]),
};

NPP_LINUX_PLUGIN_EXPORT const NppLinuxPluginInfo *npp_linux_plugin_info(void)
{
	return &info;
}
