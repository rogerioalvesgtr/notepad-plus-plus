// Linux-only lexer catalogue. The Notepad++ user-defined-language lexer
// depends on Windows and is intentionally not registered in this build.

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "ILexer.h"
#include "Lexilla.h"
#include "LexerModule.h"
#include "CatalogueModules.h"

using namespace Lexilla;

#define NPP_LEXER_DECLARATIONS \
	NPP_LEXER(A68k) NPP_LEXER(Abaqus) NPP_LEXER(Ada) NPP_LEXER(APDL) \
	NPP_LEXER(As) NPP_LEXER(Asciidoc) NPP_LEXER(Asm) NPP_LEXER(Asn1) \
	NPP_LEXER(ASY) NPP_LEXER(AU3) NPP_LEXER(AVE) NPP_LEXER(AVS) \
	NPP_LEXER(Baan) NPP_LEXER(Bash) NPP_LEXER(Batch) NPP_LEXER(BibTeX) \
	NPP_LEXER(BlitzBasic) NPP_LEXER(Bullant) NPP_LEXER(Caml) NPP_LEXER(CIL) \
	NPP_LEXER(Clw) NPP_LEXER(ClwNoCase) NPP_LEXER(Cmake) NPP_LEXER(COBOL) \
	NPP_LEXER(CoffeeScript) NPP_LEXER(Conf) NPP_LEXER(CPP) NPP_LEXER(CPPNoCase) \
	NPP_LEXER(Csound) NPP_LEXER(Css) NPP_LEXER(D) NPP_LEXER(Dart) \
	NPP_LEXER(Dataflex) NPP_LEXER(Diff) NPP_LEXER(DMAP) NPP_LEXER(DMIS) \
	NPP_LEXER(ECL) NPP_LEXER(EDIFACT) NPP_LEXER(Eiffel) NPP_LEXER(Eiffelkw) \
	NPP_LEXER(Erlang) NPP_LEXER(ErrorList) NPP_LEXER(ESCRIPT) NPP_LEXER(EscSeq) \
	NPP_LEXER(F77) NPP_LEXER(FlagShip) NPP_LEXER(Forth) NPP_LEXER(Fortran) \
	NPP_LEXER(FreeBasic) NPP_LEXER(FSharp) NPP_LEXER(GAP) NPP_LEXER(GDScript) \
	NPP_LEXER(Gui4Cli) NPP_LEXER(Haskell) NPP_LEXER(Hollywood) NPP_LEXER(HTML) \
	NPP_LEXER(IHex) NPP_LEXER(Indent) NPP_LEXER(Inno) NPP_LEXER(JSON) \
	NPP_LEXER(Julia) NPP_LEXER(Kix) NPP_LEXER(KVIrc) NPP_LEXER(Latex) \
	NPP_LEXER(LISP) NPP_LEXER(LiterateHaskell) NPP_LEXER(Lot) NPP_LEXER(Lout) \
	NPP_LEXER(Lua) NPP_LEXER(MagikSF) NPP_LEXER(Make) NPP_LEXER(Markdown) \
	NPP_LEXER(Matlab) NPP_LEXER(Maxima) NPP_LEXER(METAPOST) NPP_LEXER(MMIXAL) \
	NPP_LEXER(Modula) NPP_LEXER(MSSQL) NPP_LEXER(Nim) NPP_LEXER(Nimrod) \
	NPP_LEXER(Nix) NPP_LEXER(Nncrontab) NPP_LEXER(Nsis) NPP_LEXER(Null) \
	NPP_LEXER(Octave) NPP_LEXER(Opal) NPP_LEXER(OScript) NPP_LEXER(Pascal) \
	NPP_LEXER(PB) NPP_LEXER(Perl) NPP_LEXER(PHPSCRIPT) NPP_LEXER(PLM) \
	NPP_LEXER(PO) NPP_LEXER(POV) NPP_LEXER(PowerPro) NPP_LEXER(PowerShell) \
	NPP_LEXER(Progress) NPP_LEXER(Props) NPP_LEXER(PS) NPP_LEXER(PureBasic) \
	NPP_LEXER(Python) NPP_LEXER(R) NPP_LEXER(Raku) NPP_LEXER(REBOL) \
	NPP_LEXER(Registry) NPP_LEXER(Ruby) NPP_LEXER(Rust) NPP_LEXER(SAS) \
	NPP_LEXER(Scriptol) NPP_LEXER(SINEX) NPP_LEXER(Smalltalk) NPP_LEXER(SML) \
	NPP_LEXER(Sorc) NPP_LEXER(Specman) NPP_LEXER(Spice) NPP_LEXER(SQL) \
	NPP_LEXER(Srec) NPP_LEXER(Stata) NPP_LEXER(STTXT) NPP_LEXER(TACL) \
	NPP_LEXER(TADS3) NPP_LEXER(TAL) NPP_LEXER(TCL) NPP_LEXER(TCMD) \
	NPP_LEXER(TEHex) NPP_LEXER(TeX) NPP_LEXER(TOML) NPP_LEXER(Troff) \
	NPP_LEXER(Txt2tags) NPP_LEXER(VB) NPP_LEXER(VBScript) NPP_LEXER(Verilog) \
	NPP_LEXER(VHDL) NPP_LEXER(VisualProlog) NPP_LEXER(X12) NPP_LEXER(XML) \
	NPP_LEXER(YAML) NPP_LEXER(Zig)

#define NPP_LEXER(name) extern const LexerModule lm##name;
NPP_LEXER_DECLARATIONS
extern const LexerModule lmObjC;
extern const LexerModule lmMySQL;
extern const LexerModule lmSearchResult;
#undef NPP_LEXER

namespace
{
CatalogueModules catalogue;

void addLexers()
{
	if (catalogue.Count() != 0)
		return;
#define NPP_LEXER(name) &lm##name,
	catalogue.AddLexerModules({NPP_LEXER_DECLARATIONS &lmObjC, &lmMySQL, &lmSearchResult});
#undef NPP_LEXER
}
}

extern "C"
{
__attribute__((visibility("default"))) int GetLexerCount()
{
	addLexers();
	return static_cast<int>(catalogue.Count());
}

__attribute__((visibility("default"))) void GetLexerName(unsigned int index, char *name, int bufferLength)
{
	addLexers();
	*name = 0;
	const char *lexerName = catalogue.Name(index);
	if (static_cast<std::size_t>(bufferLength) > std::strlen(lexerName))
		std::strcpy(name, lexerName);
}

__attribute__((visibility("default"))) LexerFactoryFunction GetLexerFactory(unsigned int index)
{
	addLexers();
	return catalogue.Factory(index);
}

__attribute__((visibility("default"))) Scintilla::ILexer5 *CreateLexer(const char *name)
{
	addLexers();
	for (std::size_t i = 0; i < catalogue.Count(); ++i)
	{
		if (std::strcmp(catalogue.Name(i), name) == 0)
			return catalogue.Create(i);
	}
	return nullptr;
}

__attribute__((visibility("default"))) const char *LexerNameFromID(int identifier)
{
	addLexers();
	const LexerModule *module = catalogue.Find(identifier);
	return module ? module->languageName : nullptr;
}

__attribute__((visibility("default"))) const char *GetLibraryPropertyNames()
{
	return "";
}

__attribute__((visibility("default"))) void SetLibraryProperty(const char *, const char *)
{
}

__attribute__((visibility("default"))) const char *GetNameSpace()
{
	return "lexilla";
}
}
