# Notepad++ Linux

Uma camada de aplicação nativa para Linux Mint e Ubuntu, escrita em C++17 e GTK 3, usando o Scintilla e o Lexilla que já fazem parte deste repositório. Não é necessário Wine, Mono ou qualquer camada de compatibilidade do Windows.

**As 5 etapas planejadas estão concluídas**, e os menus seguem a mesma organização da versão Windows. Isso não significa paridade total com a versão Windows: a seção [Limites desta edição](#limites-desta-edição) lista o que continua diferente. No aplicativo, **Ajuda → Status do port** mostra o mesmo resumo.

## O que funciona

### Igual ao Windows: fechar sem perder o que não foi salvo

Ao fechar o editor (botão × da janela ou **Arquivo → Sair**), ele **não pergunta se você quer salvar**: guarda os textos de todas as abas — inclusive as sem título e as alterações ainda não salvas de arquivos existentes — e os reabre na próxima vez, como o “instantâneo de sessão” da versão Windows. As abas reabertas aparecem como não salvas (●); **Desfazer** em um arquivo recuperado volta ao conteúdo que está no disco, que nunca é alterado sem você salvar. Se o backup não puder ser gravado, o editor volta a perguntar antes de fechar. A opção fica em **Configurações → Preferências → Backup e sessão** (exige “Restaurar arquivos abertos”). Fechar uma aba isolada (Ctrl+W) continua perguntando, como no Windows.

### Igual ao Windows fora dos menus

- **Uma janela só:** abrir um arquivo pelo gerenciador de arquivos ou pelo terminal com o editor já aberto coloca o arquivo numa nova aba da janela existente, como no Windows. Assim duas cópias do editor nunca disputam a mesma sessão e os textos guardados ao sair não se perdem. Para abrir outra janela de propósito, use `--multi-instance` (ou `-multiInst`, como no Windows).
- **Arquivo alterado por outro programa:** ao voltar para o editor, ele percebe se outro programa mudou um arquivo aberto e pergunta se deve recarregar; se o arquivo foi apagado ou movido, o texto continua aberto e marcado como não salvo. Em **Preferências → Diversos** dá para desligar o aviso ou recarregar sem perguntar quando não houver alterações não salvas.
- **Botão direito:** no texto, um menu com recortar/copiar/colar, destacar ocorrências, maiúsculas, abrir arquivo selecionado, pesquisar na internet, comentários e ocultar linhas; na aba, fechar (este, os outros, à esquerda, à direita, inalterados), salvar, renomear, lixeira, recarregar, somente leitura, copiar caminho/nome/pasta e abrir a pasta. O botão do meio fecha a aba.

### Cores por linguagem, como no Windows

O realce de sintaxe usa as **mesmas cores e listas de palavras da versão Windows** (os arquivos de estilos e de linguagens do próprio Notepad++, embutidos no programa): em C/C++, por exemplo, palavras-chave em azul e negrito, tipos em roxo, números em laranja, textos em cinza, comentários em verde, pré-processador em marrom e operadores em azul-marinho; em Python o nome das funções aparece em destaque. Vale para C/C++, C#, Java, Go, JavaScript, TypeScript, Python, Rust, shell, HTML, XML, PHP, CSS, JSON, YAML, INI/TOML, SQL, Lua, Perl, Ruby, Makefile, CMake, diff, LaTeX, PowerShell, Batch, NSIS, Visual Basic e Haskell (Dart usa as cores do C++). O tema escuro usa as cores do tema escuro padrão do Windows (DarkModeDefault).

Markdown, Dockerfile, texto simples e linguagens do usuário continuam com as cores do **Configurador de estilos**. Para voltar às cores simples em todas as linguagens, desmarque **Preferências → Diversos → Cores por linguagem como no Windows**; com ela ligada, o Configurador de estilos muda o fundo, a fonte, os números de linha, a seleção e a linha atual, mas não as cores das palavras.

### Localizar, Substituir e Marcar

**Ctrl+F**, **Ctrl+H** e **Ctrl+M** abrem a janela de busca no mesmo formato da versão Windows, com as abas **Localizar**, **Substituir**, **Marcar** e **Localizar em arquivos**:

- histórico do que já foi buscado e substituído, e botão ⇅ para trocar os dois textos;
- para trás, somente palavra inteira, diferenciar maiúsculas, circular e **na seleção** (ligado sozinho quando várias linhas estão selecionadas);
- modos de busca **Normal**, **Estendido** (`\n`, `\r`, `\t`, `\0`, `\xHH`, `\uHHHH`...) e **Expressão regular**;
- localizar próximo, contar, localizar tudo no documento atual ou em todos os abertos (resultados no painel), substituir, substituir tudo (também em todos os abertos);
- **Marcar tudo** pinta as ocorrências e, com “Marcar a linha”, coloca um marcador na margem esquerda de cada linha encontrada; também limpa as marcas e copia o texto marcado;
- transparência da janela (sempre ou ao perder o foco);
- depois de usar a janela, **F3/Shift+F3** repetem a busca com as mesmas opções.

A barra de busca rápida embaixo continua disponível em **Pesquisar → Busca incremental** (Ctrl+Alt+I). Clicar nos números das linhas seleciona a linha (Shift+clique estende, arrastar seleciona várias) e clicar na faixa logo ao lado marca a linha, como no Windows.

As abas mostram o nome inteiro do arquivo; nomes com mais de 30 caracteres são encurtados no meio.

### Menus

A barra tem os mesmos menus da versão Windows, na mesma ordem: **Arquivo, Editar, Pesquisar, Exibir, Codificação, Linguagem, Configurações, Ferramentas, Macro, Executar, Plugins, Janela e Ajuda**. A lista completa de atalhos está em **Configurações → Atalhos de teclado…**. Além do que já existia, agora há:

- **Arquivo:** abrir a pasta do arquivo no gerenciador de arquivos ou no terminal, abrir no visualizador padrão, recarregar do disco, salvar uma cópia como, salvar tudo (Ctrl+Shift+S; “Salvar como” passou a Ctrl+Alt+S, como no Windows), renomear, fechar tudo (Ctrl+Shift+W), fechar todos exceto o atual / à esquerda / à direita / inalterados, mover para a lixeira, carregar e salvar sessão (arquivo XML no formato do Notepad++), imprimir (Ctrl+P) e imprimir agora.
- **Editar:** inserir data e hora (curta, longa ou no formato das Preferências), copiar caminho/nome/pasta do arquivo ou de todos os abertos, aumentar/diminuir recuo, Primeiras Maiúsculas, frase com maiúscula, iNVERTER e aLeAtÓrIo; nas operações de linha: remover duplicadas consecutivas, dividir (Ctrl+I) e juntar (Ctrl+J), remover linhas vazias, inserir linha em branco acima/abaixo, inverter, embaralhar e ordenar lexicograficamente, ignorando maiúsculas, pelo idioma, como inteiros, como decimais com vírgula ou ponto e pelo comprimento (crescente e decrescente); comentar, descomentar e comentário em bloco (Ctrl+Shift+Q); completar palavra (Ctrl+Enter) e caminho (Ctrl+Alt+Espaço); fim de linha Mac (CR); operações com espaços (início, fim, ambos, fim de linha para espaço, tabulação ↔ espaços); na seleção: abrir arquivo, abrir pasta e pesquisar na internet; selecionar todas as ocorrências ou a próxima com vários cursores; modo coluna e **editor de colunas** (Alt+C) para inserir texto ou numeração; somente leitura por documento ou em todos.
- **Pesquisar:** localizar próximo/anterior (F3/Shift+F3), selecionar e localizar (Ctrl+F3), ir para a chave correspondente (Ctrl+B), selecionar entre chaves (Ctrl+Alt+B), ir para a próxima/anterior alteração, **destacar todas as ocorrências** com 5 cores, ir até os destaques e limpá-los, e **marcadores** (Ctrl+F2 marca, F2/Shift+F2 navegam; também clicando na margem) com recortar, copiar, remover linhas marcadas ou não marcadas e inverter.
- **Exibir:** sempre visível, tela cheia (F11), modo sem distrações, ver o arquivo no navegador, mostrar todos os caracteres, guias de recuo e símbolo de quebra, navegação e movimentação de abas (Ctrl+PgUp/PgDn, Ctrl+Shift+PgUp/PgDn), **recolher e expandir blocos de código** (margem com ⊞/⊟, Alt+0, Alt+Shift+0, Alt+1…8 por nível), ocultar linhas (Alt+H), resumo do documento e **monitorar arquivo** (tail -f).
- **Codificação:** salvar em UTF-8, UTF-8 com BOM, UTF-16 LE/BE com BOM, ANSI (Windows-1252) e conjuntos de caracteres de vários idiomas (Latin-1/9, Europa central, cirílico, grego, turco, hebraico, árabe, báltico, vietnamita, tailandês, japonês, chinês e coreano); **Reabrir o arquivo como…** lê o arquivo de novo em outra codificação. A codificação de cada arquivo é detectada ao abrir (BOM, UTF-8 ou ANSI) e mantida ao salvar.
- **Linguagem:** a lista de linguagens fica direto no menu, com o submenu de linguagens definidas pelo usuário.
- **Configurações:** Preferências, Configurador de estilos, lista de atalhos e importar plugin (.so) ou linguagem do usuário.
- **Ferramentas:** MD5, SHA-1, SHA-256 e SHA-512 de um texto, de arquivos ou da seleção (copiado para a área de transferência).
- **Macro:** salvar a macro gravada com um nome (aparece no menu Macro) e executar uma macro várias vezes ou até o fim do arquivo.
- **Executar:** rodar um programa ou comando (F5) com variáveis como `$(FULL_CURRENT_PATH)`, `$(CURRENT_DIRECTORY)` e `$(CURRENT_WORD)`; abrir o arquivo no navegador; abrir um terminal na pasta do arquivo.
- **Plugins (embutidos, sem instalar nada):** **Comparar** o documento atual com outro aberto (Ctrl+Alt+C) abre os dois **lado a lado**, como o ComparePlus do Windows e a comparação do GitHub: as linhas ficam alinhadas (com linhas vazias em cinza onde um lado não tem nada), o que foi removido aparece em vermelho à esquerda, o que foi adicionado em verde à direita, e as palavras que mudaram dentro de uma linha ficam num tom mais forte. Os dois lados rolam juntos, os números são os das linhas originais (clicar num número leva até a linha no documento), a faixa à direita mostra onde estão todas as diferenças e permite pular até elas, e há botões para diferença anterior/próxima (Alt+PgUp/Alt+PgDn), atualizar (F5, também automático ao voltar para a janela depois de editar), trocar os lados, ignorar espaços ou maiúsculas e abrir o relatório no formato diff em uma nova aba. Nos documentos originais as diferenças também ficam marcadas; Ctrl+Alt+X limpa as marcas; **MIME Tools** (codificar/decodificar Base64 e URL); **Conversor** ASCII ↔ hexadecimal; **Exportar com cores** para HTML ou RTF (abre no navegador, LibreOffice ou Word com o realce); **JSMin** para minificar ou formatar JavaScript. Essas ferramentas atuam na seleção ou, sem seleção, no documento inteiro, e podem ser desfeitas com Ctrl+Z.
- **Janela:** lista dos documentos abertos para ativar, salvar ou fechar vários de uma vez, e ordenar abas por nome, caminho, tipo ou tamanho.
- **Ajuda:** argumentos de linha de comando e informações de depuração.
- Também conferidos no menu da versão Windows 8.9.7: restaurar arquivo fechado recentemente (Ctrl+Shift+T), abrir todos os recentes e limpar a lista, iniciar/terminar seleção (normal e em coluna), alternar gravação de macro (Ctrl+Shift+R), localizar (volátil) próximo/anterior (Ctrl+Alt+F3), destacar um token, copiar texto destacado, colar substituindo linhas marcadas e ordenar abas pela data de modificação.
- A margem esquerda mostra marcadores e as linhas alteradas (laranja = não salvo, verde = salvo), como no Windows; as duas margens podem ser desligadas em **Preferências → Diversos**.

### Demais recursos

- Abrir vários arquivos de uma vez, criar, salvar, salvar como e fechar com confirmação para alterações não salvas.
- Abas reordenáveis, indicação visual de alterações e lista dos dez arquivos recentes.
- Restauração dos arquivos abertos, abas sem título e alterações ainda não salvas pelo backup.
- Macros de edição: gravar, parar, reproduzir, repetir duas vezes e limpar, com persistência da última macro.
- Backup automático enquanto o editor estiver aberto (a cada 30 segundos por padrão, ajustável nas Preferências), imediatamente pelo menu Arquivo e sempre ao fechar.
- Desfazer, refazer, recortar, copiar, colar, excluir e selecionar tudo.
- Busca para frente e para trás, substituição atual ou de todas as ocorrências, com diferença de maiúsculas, palavra inteira, busca circular e expressões regulares do Scintilla (grupos escritos como `\\(grupo\\)` e referências `\\1` na substituição).
- **Localizar em arquivos** (`Ctrl+Shift+F`): busca e substituição em uma pasta, com ou sem subpastas, filtros como `*.cpp *.h`, exclusões com `!` e opção para arquivos ocultos; também busca e substituição em todos os documentos abertos.
- Painel de resultados agrupado por arquivo, com linha e coluna; duplo clique ou Enter abre o resultado, `F4`/`Shift+F4` percorrem os resultados e `F7` mostra/oculta o painel.
- **Lista de funções** (Exibir → Lista de funções): funções, classes, métodos e títulos do documento atual, com filtro, ordenação A–Z e atualização automática enquanto você digita.
- **Mapa do documento** (Exibir → Mapa do documento): miniatura do arquivo com a área visível destacada; clique, arraste ou use a roda do mouse para navegar.
- **Espaço de trabalho** (Exibir → Espaço de trabalho): árvore com as pastas do projeto, abertura de arquivos por duplo clique, busca na pasta selecionada e arquivos `.npp-workspace` para salvar e reabrir o conjunto de pastas.
- Ir para uma linha e visualização da posição, linguagem, codificação e fim de linha.
- Detecção da codificação ao abrir (UTF-8, UTF-8 com BOM, UTF-16 com BOM ou ANSI), gravação na mesma codificação, conversão CRLF/LF/CR e salvamento por substituição atômica no mesmo diretório.
- Realce de sintaxe configurado para C/C++, Python, JavaScript/TypeScript, Java/C#, Rust, Go, shell, HTML/XML, CSS, JSON, YAML, TOML/INI, Markdown, SQL, Lua, Perl, Ruby, PHP, Make, CMake, Dockerfile, diff, LaTeX, PowerShell, Batch, NSIS, Visual Basic, Haskell e Dart.
- Temas claro e escuro, fonte e tamanho configuráveis, tabulações ou espaços, números de linha, espaços visíveis, quebra de linha e zoom.
- **Configurar estilos e cores** (Formato): cor do texto e do fundo, negrito e itálico para texto, comentários, palavras-chave, textos, números, operadores, números de linha, linha atual, seleção e par de chaves, separadamente para o tema claro e o escuro.
- **Preferências** em abas: recuo automático, fechamento automático de ( [ {, destaque da linha atual e do par de chaves, guias de recuo, linha-guia numa coluna, fim de linha e linguagem dos novos documentos, intervalo do backup e quantidade de arquivos recentes.
- **Linguagens definidas pelo usuário** (Formato): criar, editar, importar e exportar no formato XML de linguagens do Notepad++, com realce de palavras-chave, comentários, textos, números e operadores.
- **Plugins nativos para Linux** (menu Plugins), com interface em C documentada e um plugin de exemplo.
- **Operações de linha** (Editar): duplicar, excluir, mover, ordenar de A a Z ou de Z a A, remover duplicadas, remover espaços no fim, MAIÚSCULAS/minúsculas e comentar/descomentar linhas.
- Atalhos familiares: Ctrl+N/O/S/W/F/H/G/A/Z/Y/X/C/V, Ctrl+D (duplicar linha), Ctrl+Shift+L (excluir linha), Ctrl+Shift+↑/↓ (mover linha), Ctrl+U / Ctrl+Shift+U (minúsculas/maiúsculas), Ctrl+K (comentar), F1 e Ctrl+Q (sair). No Linux, Ctrl+Q continua sendo “Sair”; “alternar comentário” fica em Ctrl+K.

## Requisitos

Ubuntu 22.04 ou mais recente, Linux Mint 21 ou mais recente, compilador C++17, Make e GTK 3. Em uma instalação limpa:

```sh
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev
```

Nenhuma dependência é baixada ou instalada automaticamente pelo build.

## Compilar e executar

```sh
make -C linux -j"$(nproc)"
./linux/bin/notepad-plus-plus-linux [--no-session] [--multi-instance] [arquivo ...]
```

O build gera bibliotecas estáticas locais a partir dos componentes incluídos no projeto e um executável ELF nativo. O código original do Notepad++ para Windows permanece separado.

## Testar

Com um ambiente gráfico disponível:

```sh
make -C linux check
```

O teste cria uma janela GTK real e exercita documentos, texto Unicode, realce C++, busca/substituição, busca e substituição em pastas, lista de funções, mapa do documento, espaço de trabalho, estilos, preferências, linguagens do usuário, operações de linha, o plugin de exemplo, macros, recuperação, navegação, salvamento/leitura, abas, conversões de fim de linha, desfazer/refazer, preferências e ISO-8859-1. Ele usa uma pasta isolada dentro de `linux/build/self-test` e não escreve nas configurações pessoais. O mesmo alvo executa duas instâncias consecutivas com `XDG_CONFIG_HOME` isolado para confirmar que uma aba sem título é restaurada por um novo processo.

Para um computador sem interface gráfica, instale `xvfb` e execute:

```sh
xvfb-run -a make -C linux check
```

## Instalar no menu de aplicativos

```sh
sudo make -C linux install
notepad-plus-plus-linux
```

Por padrão instala em `/usr/local`. É possível usar outro prefixo:

```sh
make -C linux install PREFIX="$HOME/.local"
```

Para remover o que foi instalado pelo Makefile, use `sudo make -C linux uninstall` (ou o mesmo `PREFIX` utilizado).

## Gerar e instalar o pacote .deb

O pacote permite instalar o editor no Linux Mint ou Ubuntu com dois cliques, sem compilar no computador de destino.

O que é preciso no computador que gera o pacote: os mesmos pacotes da compilação mais o `dpkg-dev`.

```sh
sudo apt install build-essential pkg-config libgtk-3-dev dpkg-dev
make -C linux deb
```

O arquivo fica em `linux/bin/notepad-plus-plus-linux_0.7.0-1_amd64.deb`. Para instalar, dê dois cliques nele (o Instalador de Pacotes do Mint resolve as dependências) ou use `sudo apt install ./linux/bin/notepad-plus-plus-linux_0.7.0-1_amd64.deb`. Para remover: `sudo apt remove notepad-plus-plus-linux`.

- **Compatibilidade:** o pacote exige bibliotecas iguais ou mais novas que as do computador onde foi gerado. Um pacote gerado no Linux Mint 22 / Ubuntu 24.04 funciona nessas versões e nas mais novas, mas não no Mint 21 / Ubuntu 22.04. Para atender versões antigas, gere o pacote na versão mais antiga que quiser suportar.
- As dependências são detectadas automaticamente pelo `dpkg-shlibdeps` (do `dpkg-dev`); sem ele é usada uma lista genérica.
- Variáveis opcionais: `VERSION`, `DEB_REVISION` e `MAINTAINER`, por exemplo `make -C linux deb MAINTAINER="Seu Nome <voce@exemplo.com>"`.
- O pacote inclui o editor, o atalho no menu, o ícone, o plugin de exemplo (desativado até você ativá-lo), o cabeçalho para escrever plugins, a licença e este guia.
- O pacote não é assinado nem publicado em repositório; quem recebe o arquivo precisa confiar na origem dele.

## Macros e recuperação

Use **Macro → Iniciar gravação**, edite o documento e escolha **Parar gravação**. **Reproduzir macro** (`Ctrl+Shift+P`) repete os comandos no documento atual; **Repetir duas vezes** executa duas repetições em uma única ação de desfazer. A última macro fica em `last-macro.xml` e é validada antes da reprodução para aceitar apenas comandos de edição e navegação.

O timer de backup existe somente enquanto o aplicativo está aberto. **Arquivo → Salvar backup agora** também grava a sessão e os documentos alterados, e fechar o editor sempre grava um backup final. Cada aba sem título usa um arquivo próprio em `backups/`; nome, conteúdo, linguagem e codificação são restaurados na próxima abertura. Alterações de arquivos existentes são recuperadas no editor sem alterar automaticamente o original. Salvar ou descartar explicitamente a aba remove o backup correspondente; se o documento salvo for editado novamente, um backup novo protege essas alterações posteriores.

**Macro → Salvar macro gravada…** guarda a macro com um nome em `macros/` (ela aparece no fim do menu Macro) e **Executar macro várias vezes…** repete a macro atual ou uma salva um número de vezes ou até o fim do arquivo (com limite de 30 segundos). Um encerramento abrupto (falta de energia, o sistema matar o processo) antes do próximo backup automático pode perder as edições mais recentes; fechar normalmente não perde nada.

## Localizar em arquivos

Abra **Pesquisar → Localizar em arquivos…** (`Ctrl+Shift+F`) ou o botão **Em arquivos** da barra de ferramentas. Escolha a pasta, digite o texto e use:

- **Localizar todos**: lista as ocorrências na pasta escolhida.
- **Substituir em arquivos**: pede confirmação e grava os arquivos fechados no disco. Essa gravação não pode ser desfeita pelo editor.
- **Localizar nos abertos** / **Substituir nos abertos**: atuam em todas as abas, inclusive sem título.

Filtros aceitam padrões separados por espaço, vírgula ou ponto e vírgula (`*.cpp *.h`). Um `!` exclui arquivos (`!*.min.js`) ou pastas (`!build/`, `!\build` ou `!+\node_modules`). A comparação dos filtros ignora maiúsculas.

Regras de segurança da substituição:

- Arquivos já abertos são alterados apenas no editor (podem ser desfeitos e precisam ser salvos); a busca também usa o conteúdo ainda não salvo dessas abas.
- Arquivos fechados são gravados por substituição atômica, mantendo permissões, BOM UTF-8 e fins de linha. Atalhos simbólicos são preservados e o arquivo de destino é alterado.
- Arquivos que não são UTF-8, somente leitura, alterados durante a operação, binários ou maiores que 64 MB não são modificados; os motivos aparecem no painel de resultados.
- Pastas e arquivos ocultos ficam de fora, a menos que a opção seja marcada. Atalhos para pastas não são seguidos.

A busca roda de uma vez e mostra no máximo 50.000 ocorrências e 100.000 arquivos; em pastas muito grandes a janela pode ficar ocupada por alguns segundos.

## Painéis de navegação

Os três painéis são ligados e desligados no menu **Exibir** e ficam abertos na próxima sessão.

- **Lista de funções** reconhece C/C++, Java/C#, JavaScript/TypeScript, Rust, Go, PHP, Dart e Python (classes, métodos e funções), além de funções em shell, Lua, Perl, Ruby, PowerShell, Batch, NSIS, Visual Basic, Haskell, CMake e SQL, alvos de Makefile, estágios de Dockerfile, títulos Markdown e LaTeX, seções TOML/INI, chaves YAML, seletores CSS e arquivos de um diff. Comentários e textos entre aspas são ignorados. A análise é aproximada: não compila o código e macros incomuns podem aparecer ou ficar de fora. Duplo clique leva até a definição.
- **Mapa do documento** mostra o arquivo atual em miniatura ao lado do editor e acompanha a troca de abas, a rolagem e as edições. O mapa não pode ser editado.
- **Espaço de trabalho** guarda uma ou mais pastas de projeto. Os botões do painel adicionam (+), removem (−, sem apagar nada do disco), atualizam e abrem **Localizar em arquivos** na pasta selecionada. Arquivos e pastas ocultos não aparecem. **Arquivo → Salvar espaço de trabalho como…** grava a lista de pastas em um arquivo `.npp-workspace`, que pode ser reaberto em **Arquivo → Abrir espaço de trabalho…**.

## Linguagens definidas pelo usuário

Em **Formato → Linguagens definidas pelo usuário…** é possível criar uma linguagem com nome, extensões, comentário de linha, comentário em bloco, até quatro grupos de palavras-chave, operadores e delimitador de texto. **Importar** aceita os arquivos XML exportados pelo Notepad++ para Windows (`userDefineLang.xml` ou arquivos de `userDefineLangs`); **Exportar** gera um XML no mesmo formato.

São lidos: extensões, diferenciação de maiúsculas, comentários de linha e em bloco, os oito grupos de palavras-chave, operadores, até oito delimitadores (com escape e fechamento no fim da linha) e as cores do arquivo. Dobra de código, prefixos/sufixos de números, operadores do grupo 2 e aninhamento de estilos não são aplicados. As cores salvas na linguagem são usadas no tema claro; no tema escuro as cores do tema são usadas para manter a leitura.

As definições ficam em `~/.config/notepad-plus-plus/userDefineLangs/`. Remover uma linguagem move o arquivo para `userDefineLangs/removidas/`, sem apagá-lo.

## Plugins nativos

Plugins são bibliotecas `.so` compiladas para Linux. O editor procura em:

- `~/.local/share/notepad-plus-plus/plugins/` (como `nome.so` ou `nome/nome.so`);
- `<prefixo>/lib/notepad-plus-plus-linux/plugins/` (instalado pelo `make install`, onde fica o plugin de exemplo **Ferramentas de texto**).

**Um plugin novo fica desativado até você ativá-lo em Plugins → Gerenciar plugins.** Plugins rodam dentro do editor com as suas permissões; ative apenas os de fontes confiáveis. Arquivos que qualquer usuário pode alterar são recusados. Desativar um plugin vale a partir da próxima abertura do editor.

Para escrever um plugin, inclua `include/npp-linux-plugin.h` (instalado em `<prefixo>/include`), exporte `npp_linux_plugin_info` e compile com `cc -shared -fPIC`. O cabeçalho descreve as funções oferecidas pelo editor: ler e substituir texto e seleção, enviar mensagens ao Scintilla, abrir arquivos, criar documentos e mostrar mensagens. O exemplo completo está em `plugins/text-tools/text-tools.c`.

Plugins DLL do Windows não podem ser carregados: eles dependem da API Win32 e precisam ser reescritos ou recompilados para esta interface.

## Preferências e sessão

Os arquivos são guardados em `$XDG_CONFIG_HOME/notepad-plus-plus/` ou `~/.config/notepad-plus-plus/`:

- `settings.xml`: tema, cores personalizadas, fonte, opções do editor, arquivos recentes, última pasta/filtros da busca em arquivos, painéis abertos, pastas do espaço de trabalho e plugins ativados.
- `userDefineLangs/`: linguagens definidas pelo usuário.
- `session.txt`: índice da sessão, com identidades de recuperação.
- `backups/`: conteúdo dos documentos recuperáveis, com permissão somente para o usuário.
- `last-macro.xml`: última macro gravada.
- `macros/`: macros salvas com nome.

## Faltam etapas?

Não. **As 5 etapas planejadas estão concluídas.** Ainda há diferenças em relação à versão Windows, listadas em Limites desta edição.

| Etapa | Situação | Escopo |
| --- | --- | --- |
| 1 | Concluída | Editor nativo, abas, arquivos, busca/substituição no documento, realce, preferências básicas e sessão. |
| 2 | Concluída | Macros, gravação/reprodução de comandos, backup periódico e recuperação de documentos sem título. |
| 3 | Concluída | Localizar e substituir em vários arquivos e pastas, filtros e resultados navegáveis. |
| 4 | Concluída | Lista de funções, mapa do documento, gerenciador de projetos e painéis de navegação. |
| 5 | Concluída | Linguagens definidas pelo usuário, preferências e estilos completos, sistema de plugins nativos e demais ferramentas. |

**Plugins DLL do Windows não executam nativamente no Linux.** A compatibilidade com o ecossistema de plugins exige versões Linux desses plugins ou uma camada de adaptação própria; não basta apenas recompilar a interface. Wine continua fora do escopo.

## Limites desta edição

- Não há compatibilidade com plugins DLL do Windows, Plugin Admin (catálogo de plugins para baixar), projetos com pastas virtuais no formato do Notepad++ para Windows, FTP, ferramentas administrativas específicas do Windows.
- Itens dos menus do Windows que ainda não existem aqui: segunda vista (mover/clonar para a outra vista, dividir a tela e sincronizar rolagem), abrir em nova instância, Post-it, direção do texto da direita para a esquerda, cores nas abas, colar HTML/RTF e conteúdo binário, ocultar com █ (redact), painel de caracteres, histórico da área de transferência, completar funções e dicas de parâmetros (dependem dos arquivos de API do Windows), “Marcar…”, busca incremental, localizar caracteres por faixa, limpar o histórico de alterações, “Somente leitura” do sistema de arquivos, troca de atalhos (Mapeador de atalhos), importar temas, editar o menu de contexto, salvar comandos no menu Executar, atualização automática e links da internet do menu Ajuda.
- A lista acima foi conferida com os menus do Notepad++ 8.9.7 para Windows. As funções principais dos plugins ComparePlus, MIME Tools, NppConverter, NppExport e JSMin vêm embutidas (veja abaixo); outros plugins do Windows não existem aqui.
- A comparação lado a lado abre numa janela própria e só para leitura: para editar, clique no número da linha e altere o documento original (a comparação se atualiza ao voltar). Linhas movidas de lugar aparecem como removidas e adicionadas, e linguagens definidas pelo usuário aparecem sem cores na comparação.
- A impressão usa a fonte do editor em preto e branco, com o nome do arquivo e o número da página no topo; ainda não imprime as cores do realce.
- As linguagens do usuário usam um realce próprio, compatível com o formato XML do Notepad++, mas sem dobra de código nem todas as opções avançadas do editor de linguagens do Windows.
- O configurador de estilos cobre as categorias comuns a todas as linguagens; ainda não há cores separadas por linguagem nem importação de temas `.xml` do Notepad++.
- As Preferências reúnem as opções mais usadas (incluindo codificação de novos documentos, formato da data, site de pesquisa e margens); muitas opções específicas do Windows não existem nesta edição.
- Um plugin com defeito pode fechar o editor, porque roda no mesmo processo; o backup automático reduz a perda de trabalho.
- O backup restaura o último conteúdo salvo pelo timer ou por encerramento normal; ele não substitui o salvamento do documento nem garante recuperação após exclusão dos arquivos de configuração.
- A aplicação foi compilada e testada no Linux x86-64 disponível neste ambiente. Outras arquiteturas e versões do Mint/Ubuntu ainda exigem validação.

O projeto segue a licença do repositório original: GPL versão 3, com os esclarecimentos descritos no arquivo `LICENSE` do Notepad++. Scintilla e Lexilla mantêm a licença própria deles.
