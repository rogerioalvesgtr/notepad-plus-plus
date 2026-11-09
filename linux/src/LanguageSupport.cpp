#include "LanguageSupport.h"

#include <algorithm>
#include <cctype>

#include <glib.h>

namespace NppLinux
{
namespace
{
const std::vector<Language> languageList
{
	{"text", "Texto simples", "", {}, "", {}, {}, {}, {}, {}},
	{"cpp", "C / C++", "cpp", {"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx"},
		"alignas alignof auto bool break case catch char class const constexpr const_cast continue decltype default delete do double dynamic_cast else enum explicit export extern false final float for friend goto if inline int long mutable namespace new noexcept nullptr operator override private protected public register reinterpret_cast return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while",
		{2}, {1, 3, 15, 17, 18}, {6, 7, 8, 10, 11, 12}, {4}, {10}},
	{"python", "Python", "python", {"py", "pyw"},
		"False None True and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield match case",
		{1}, {1, 12}, {3, 4, 6, 7, 13, 14}, {2}, {10}},
	{"javascript", "JavaScript / TypeScript", "cpp", {"js", "jsx", "mjs", "cjs", "ts", "tsx"},
		"async await break case catch class const continue debugger default delete do else export extends false finally for function if implements import in instanceof interface let new null of package private protected public return static super switch this throw true try typeof undefined var void while with yield type enum namespace declare satisfies",
		{2}, {1, 3, 15, 17, 18}, {6, 7, 8, 10, 11, 12}, {4}, {10}},
	{"java", "Java / C#", "cpp", {"java", "cs"},
		"abstract as async await base bool break byte case catch char checked class const continue decimal default delegate do double else enum event explicit extern false finally fixed float for foreach goto if implicit in int interface internal is lock long namespace new null object operator out override params private protected public readonly record ref return sbyte sealed short sizeof stackalloc static string struct switch this throw true try typeof uint ulong unchecked unsafe ushort using var virtual void volatile while",
		{2}, {1, 3, 15, 17, 18}, {6, 7, 8, 10, 11, 12}, {4}, {10}},
	{"rust", "Rust", "rust", {"rs"},
		"as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while union",
		{2}, {}, {}, {}, {}},
	{"shell", "Shell script", "bash", {"sh", "bash", "zsh"},
		"if then else elif fi for while until do done case esac function in select time coproc return break continue local export readonly declare typeset unset shift trap source alias eval exec exit set",
		{}, {}, {}, {}, {}},
	{"html", "HTML / XML", "hypertext", {"html", "htm", "xhtml", "xml", "svg", "vue"},
		"html head body title meta link script style div span main section article aside header footer nav h1 h2 h3 p a img form input button label select option table thead tbody tr th td ul ol li xml version encoding",
		{}, {}, {}, {}, {}},
	{"css", "CSS / SCSS", "css", {"css", "scss", "less"}, "", {}, {}, {}, {}, {}},
	{"json", "JSON", "json", {"json", "jsonc", "jsonl", "geojson"}, "", {}, {}, {}, {}, {}},
	{"yaml", "YAML", "yaml", {"yaml", "yml"}, "true false null yes no on off", {}, {}, {}, {}, {}},
	{"toml", "TOML / INI", "props", {"toml", "ini", "cfg", "conf", "properties", "editorconfig"},
		"true false", {4}, {1}, {3}, {2}, {0}},
	{"markdown", "Markdown", "markdown", {"md", "markdown"}, "", {}, {}, {}, {}, {}},
	{"sql", "SQL", "sql", {"sql"},
		"select from where insert into values update set delete create alter drop table view index database join inner left right full outer on as and or not null primary key foreign references group by order having limit offset union all distinct with begin commit rollback transaction",
		{5}, {1, 2, 13, 15, 23}, {6, 7}, {3}, {4}},
	{"lua", "Lua", "lua", {"lua"},
		"and break do else elseif end false for function goto if in local nil not or repeat return then true until while",
		{3}, {1, 2}, {4, 8}, {5}, {10}},
	{"perl", "Perl", "perl", {"pl", "pm", "t"},
		"my our local sub use require package if elsif else unless while until for foreach do last next redo return defined undef and or not eq ne lt le gt ge",
		{}, {}, {}, {}, {}},
	{"ruby", "Ruby", "ruby", {"rb", "rake", "gemspec"},
		"alias and begin break case class def defined? do else elsif end ensure false for if in module next nil not or redo rescue retry return self super then true undef unless until when while yield require require_relative attr_accessor attr_reader attr_writer",
		{4}, {1}, {6, 7, 10}, {3}, {11}},
	{"php", "PHP", "phpscript", {"php", "phtml"},
		"abstract and array as break callable case catch class clone const continue declare default die do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile eval exit extends final finally fn for foreach function global goto if implements include include_once instanceof insteadof interface isset list match namespace new or print private protected public readonly require require_once return static switch throw trait try unset use var while xor yield true false null",
		{}, {}, {}, {}, {}},
	{"go", "Go", "cpp", {"go"},
		"break default func interface select case defer go map struct chan else goto package switch const fallthrough if range type continue for import return var bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr true false nil iota any comparable",
		{2}, {1, 3, 15, 17, 18}, {6, 7, 8, 10, 11, 12}, {4}, {10}},
	{"make", "Makefile", "makefile", {"mk", "mak"}, "", {}, {}, {}, {}, {}},
	{"cmake", "CMake", "cmake", {"cmake"}, "cmake_minimum_required project add_executable add_library target_link_libraries target_include_directories find_package set unset option if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro message include install enable_testing add_test", {}, {}, {}, {}, {}},
	{"dockerfile", "Dockerfile", "props", {"dockerfile"}, "FROM RUN CMD LABEL EXPOSE ENV ADD COPY ENTRYPOINT VOLUME USER WORKDIR ARG ONBUILD STOPSIGNAL HEALTHCHECK SHELL", {4}, {1}, {3}, {2}, {0}},
	{"diff", "Diff / patch", "diff", {"diff", "patch"}, "", {}, {}, {}, {}, {}},
	{"tex", "LaTeX / BibTeX", "latex", {"tex", "bib", "sty", "cls"}, "", {}, {}, {}, {}, {}},
	{"powershell", "PowerShell", "powershell", {"ps1", "psm1", "psd1"}, "function param if elseif else foreach for while switch do until try catch finally throw return begin process end class enum filter workflow in continue break exit trap true false null", {}, {}, {}, {}, {}},
	{"batch", "Batch", "batch", {"bat", "cmd"}, "call cd chdir cls copy date del dir echo else endlocal erase exit for goto if md mkdir move path pause popd prompt pushd rd rem ren rename rmdir set setlocal shift start time title type ver verify vol", {}, {}, {}, {}, {}},
	{"nsis", "NSIS", "nsis", {"nsi", "nsh"}, "", {}, {}, {}, {}, {}},
	{"vb", "Visual Basic", "vb", {"vb", "vbs", "bas"}, "and as boolean byref byval call case class const dim do each else elseif end enum erase exit false for function get if in is let loop me mod new next not nothing null on option or private public redim resume select set static stop sub then to true until wend while with", {}, {}, {}, {}, {}},
	{"haskell", "Haskell", "haskell", {"hs"}, "case class data default deriving do else foreign if import in infix infixl infixr instance let module newtype of then type where qualified hiding as", {}, {}, {}, {}, {}},
	{"dart", "Dart", "cpp", {"dart"}, "abstract as assert async await break case catch class const continue covariant default deferred do dynamic else enum export extends extension external factory false final finally for get hide if implements import in interface is late library mixin new null on operator part required rethrow return set show static super switch sync this throw true try typedef var void when while with yield", {2}, {1, 3, 15, 17, 18}, {6, 7, 8, 10, 11, 12}, {4}, {10}}
};

std::string lowercase(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return text;
}
}

const std::vector<Language> &languages()
{
	return languageList;
}

const Language &plainTextLanguage()
{
	return languageList.front();
}

const Language &languageForPath(const std::filesystem::path &path)
{
	std::string filename = lowercase(path.filename().string());
	if (filename == "makefile" || filename == "gnumakefile")
		return *std::find_if(languageList.begin(), languageList.end(), [](const Language &language) {
			return language.id == "make";
		});
	if (filename == "cmakelists.txt")
		filename = "cmakelists.cmake";
	if (filename == "dockerfile")
		return *std::find_if(languageList.begin(), languageList.end(), [](const Language &language) {
			return language.id == "dockerfile";
		});
	const std::string extension = lowercase(path.extension().string());
	for (const auto &language : languageList)
	{
		for (const auto &candidate : language.extensions)
		{
			if (extension == "." + candidate)
				return language;
		}
	}
	return plainTextLanguage();
}

std::string trimBom(std::string bytes)
{
	if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF
		&& static_cast<unsigned char>(bytes[1]) == 0xBB
		&& static_cast<unsigned char>(bytes[2]) == 0xBF)
		bytes.erase(0, 3);
	return bytes;
}

std::string normaliseUtf8(const std::string &bytes, std::string &encoding)
{
	std::string text = trimBom(bytes);
	if (g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr))
	{
		encoding = "UTF-8";
		return text;
	}
	gchar *converted = g_convert(text.c_str(), static_cast<gssize>(text.size()), "UTF-8", "ISO-8859-1", nullptr, nullptr, nullptr);
	if (!converted)
		return text;
	std::string result = converted;
	g_free(converted);
	encoding = "ISO-8859-1 → UTF-8";
	return result;
}
}
