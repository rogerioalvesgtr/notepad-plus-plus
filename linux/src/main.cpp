#include "EditorWindow.h"
#include "Integration.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
void printHelp(const char *program)
{
	std::cout << "Notepad++ Linux - editor nativo para Ubuntu e Linux Mint\n\n"
		<< "Uso: " << program << " [opções] [arquivos...]\n\n"
		<< "  --help                 Mostra esta ajuda\n"
		<< "  --version             Mostra a versão\n"
		<< "  --no-session          Não restaura a sessão anterior nesta execução\n"
		<< "  --multi-instance      Abre outra janela em vez de usar a que já está aberta\n"
		<< "  --self-test <pasta>   Testa uma janela GTK, edição, busca e arquivos\n"
		<< "  --recovery-smoke-test <pasta>  Testa recuperação entre duas execuções\n";
}
}

int main(int argc, char **argv)
{
	std::filesystem::path selfTestDirectory;
	bool noSession = false;
	bool recoverySmokeTest = false;
	bool multiInstance = false;
	std::vector<std::filesystem::path> paths;
	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];
		if (argument == "--help" || argument == "-h")
		{
			printHelp(argv[0]);
			return EXIT_SUCCESS;
		}
		if (argument == "--version")
		{
			std::cout << "Notepad++ Linux 0.7.0\n";
			return EXIT_SUCCESS;
		}
		if (argument == "--recovery-smoke-test" && i + 1 < argc)
		{
			recoverySmokeTest = true;
			noSession = true;
			selfTestDirectory = argv[++i];
			continue;
		}
		if (argument == "--multi-instance" || argument == "-multiInst")
		{
			multiInstance = true;
			continue;
		}
		if (argument == "--no-session")
		{
			noSession = true;
			continue;
		}
		if (argument == "--self-test" && i + 1 < argc)
		{
			selfTestDirectory = argv[++i];
			continue;
		}
		if (!argument.empty() && argument.front() == '-')
		{
			std::cerr << "Opção desconhecida: " << argument << '\n';
			printHelp(argv[0]);
			return EXIT_FAILURE;
		}
		paths.emplace_back(argument);
	}

	// Like the Windows version, files opened while the editor is running go to
	// the open window instead of starting a second editor.
	const bool singleInstance = selfTestDirectory.empty() && !multiInstance;
	if (singleInstance && NppLinux::sendToRunningInstance(NppLinux::instanceSocketPath(), paths))
		return EXIT_SUCCESS;
	gtk_init(&argc, &argv);
	NppLinux::EditorWindow window(!selfTestDirectory.empty() && !recoverySmokeTest, noSession, recoverySmokeTest);
	for (const auto &path : paths)
		window.openPath(path);
	if (recoverySmokeTest)
	{
		std::string result;
		const bool success = window.runRecoverySmokeTest(selfTestDirectory, result);
		std::cout << result;
		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	if (!selfTestDirectory.empty())
	{
		std::string result;
		const bool success = window.runSelfTest(selfTestDirectory, result);
		std::cout << result;
		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	if (singleInstance)
		window.startInstanceServer(NppLinux::instanceSocketPath());
	window.show();
	gtk_main();
	return EXIT_SUCCESS;
}
