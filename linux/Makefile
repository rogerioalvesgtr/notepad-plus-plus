# Native Linux build for Ubuntu and Linux Mint.
# The Windows application and the vendored libraries are left independent.

CXX ?= g++
CC ?= cc
PKG_CONFIG ?= pkg-config
BUILD_DIR ?= build
BIN_DIR ?= bin
TARGET ?= $(BIN_DIR)/notepad-plus-plus-linux
PREFIX ?= /usr/local
PLUGIN_DIR ?= $(PREFIX)/lib/notepad-plus-plus-linux/plugins

CPPFLAGS += -include cstdint -DGTK -I../scintilla/include -I../lexilla/include -Isrc -Iinclude -DNPP_LINUX_PLUGIN_DIR='"$(PLUGIN_DIR)"'
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDLIBS += -pthread -ldl
# Installation and cleanup of existing artifacts do not need development headers.
ifneq ($(filter install uninstall clean,$(MAKECMDGOALS)),)
else
GTK_FLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 gmodule-no-export-2.0)
endif

SOURCES := src/main.cpp src/EditorWindow.cpp src/EditorDocument.cpp src/LanguageSupport.cpp src/Search.cpp src/FileSearch.cpp src/FunctionList.cpp src/Workspace.cpp src/Theme.cpp src/UserLanguage.cpp src/PluginManager.cpp src/LineTools.cpp src/Settings.cpp src/SessionStore.cpp src/TextTools.cpp src/MenuCommands.cpp src/BuiltinTools.cpp src/Integration.cpp src/FindDialog.cpp src/NppStyles.cpp src/CompareView.cpp
OBJECTS := $(SOURCES:src/%.cpp=$(BUILD_DIR)/%.o)
# The colours and keyword lists of the Windows version, built into the program.
NPP_STYLE_FILES := ../PowerEditor/src/stylers.model.xml ../PowerEditor/installer/themes/DarkModeDefault.xml ../PowerEditor/src/langs.model.xml
NPP_STYLE_DATA := $(BUILD_DIR)/NppStyleData.o
DEPS := $(OBJECTS:.o=.d)
SCINTILLA := ../scintilla/bin/scintilla.a
LEXILLA := $(BUILD_DIR)/liblexilla.a
LEXILLA_OBJECTS := $(filter-out ../lexilla/src/Lexilla.o ../lexilla/src/LexUser.o,$(wildcard ../lexilla/src/*.o))
SAMPLE_PLUGIN := $(BUILD_DIR)/plugins/text-tools/text-tools.so

.PHONY: all clean check install uninstall libraries plugins
all: $(TARGET) plugins

plugins: $(SAMPLE_PLUGIN)

libraries:
	$(MAKE) -C ../scintilla/gtk GTK3=1 static PKG_CONFIG="$(PKG_CONFIG)"
	$(MAKE) $(LEXILLA)

$(SCINTILLA): $(wildcard ../scintilla/gtk/*.cxx ../scintilla/src/*.cxx)
	$(MAKE) -C ../scintilla/gtk GTK3=1 static PKG_CONFIG="$(PKG_CONFIG)"

$(LEXILLA): lexers/LexillaLinux.cxx $(wildcard ../lexilla/lexers/*.cxx ../lexilla/lexlib/*.cxx) | $(BUILD_DIR)
	$(MAKE) -C ../lexilla/src ../bin/liblexilla.a CPPFLAGS="-include cstdint" LEXERS="$(filter-out LexUser.cxx,$(notdir $(wildcard ../lexilla/lexers/Lex*.cxx)))"
	rm -f $@
	$(AR) rc $@ $(LEXILLA_OBJECTS)
	$(CXX) -include cstdint -std=c++17 -fPIC -I../scintilla/include -I../lexilla/include -I../lexilla/lexlib -c lexers/LexillaLinux.cxx -o $(BUILD_DIR)/LexillaLinux.o
	$(AR) rc $@ $(BUILD_DIR)/LexillaLinux.o
	ranlib $@

$(TARGET): $(OBJECTS) $(NPP_STYLE_DATA) $(SCINTILLA) $(LEXILLA) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OBJECTS) $(NPP_STYLE_DATA) $(SCINTILLA) $(LEXILLA) -o $@ $(GTK_LIBS) $(LDLIBS)

$(BUILD_DIR)/NppStyleData.cpp: $(NPP_STYLE_FILES) | $(BUILD_DIR)
	{ printf 'namespace NppLinux {\nextern const char nppStylersLight[] = R"nppxml('; cat ../PowerEditor/src/stylers.model.xml; \
	  printf ')nppxml";\nextern const char nppStylersDark[] = R"nppxml('; cat ../PowerEditor/installer/themes/DarkModeDefault.xml; \
	  printf ')nppxml";\nextern const char nppLanguages[] = R"nppxml('; cat ../PowerEditor/src/langs.model.xml; \
	  printf ')nppxml";\n}\n'; } > $@.tmp && mv $@.tmp $@

# Long string literals are fine for GCC and Clang; -Wpedantic would only warn about them.
$(NPP_STYLE_DATA): $(BUILD_DIR)/NppStyleData.cpp
	$(CXX) -std=c++17 -O1 -c $< -o $@

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GTK_FLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Example native plugin; also used by the self-test.
$(SAMPLE_PLUGIN): plugins/text-tools/text-tools.c include/npp-linux-plugin.h
	mkdir -p $(dir $@)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -fPIC -shared -fvisibility=hidden -Iinclude -I../scintilla/include $< -o $@

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

check: $(TARGET) $(SAMPLE_PLUGIN)
	$(TARGET) --self-test $(BUILD_DIR)/self-test
	rm -rf $(abspath $(BUILD_DIR)/recovery-smoke)
	# Same as a person who had turned off "restore open files" in an older version.
	mkdir -p $(abspath $(BUILD_DIR)/recovery-smoke/config/notepad-plus-plus)
	printf '<?xml version="1.0" encoding="UTF-8"?>\n<NotepadPlusPlusLinux version="1">\n\t<restoreSession>false</restoreSession>\n</NotepadPlusPlusLinux>\n' > $(abspath $(BUILD_DIR)/recovery-smoke/config/notepad-plus-plus/settings.xml)
	XDG_CONFIG_HOME="$(abspath $(BUILD_DIR)/recovery-smoke/config)" $(TARGET) --recovery-smoke-test $(abspath $(BUILD_DIR)/recovery-smoke)
	XDG_CONFIG_HOME="$(abspath $(BUILD_DIR)/recovery-smoke/config)" $(TARGET) --recovery-smoke-test $(abspath $(BUILD_DIR)/recovery-smoke)

install: $(TARGET) $(SAMPLE_PLUGIN)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/notepad-plus-plus-linux
	install -Dm644 packaging/notepad-plus-plus-linux.desktop $(DESTDIR)$(PREFIX)/share/applications/notepad-plus-plus-linux.desktop
	install -Dm644 packaging/notepad-plus-plus-linux.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/notepad-plus-plus-linux.svg
	install -Dm644 packaging/notepad-plus-plus-linux.metainfo.xml $(DESTDIR)$(PREFIX)/share/metainfo/notepad-plus-plus-linux.metainfo.xml
	install -Dm644 $(SAMPLE_PLUGIN) $(DESTDIR)$(PLUGIN_DIR)/text-tools/text-tools.so
	install -Dm644 include/npp-linux-plugin.h $(DESTDIR)$(PREFIX)/include/npp-linux-plugin.h

# Debian/Ubuntu/Linux Mint package: make -C linux deb
# The package works on systems with the same or newer libraries than the build machine,
# so build it on the oldest release you want to support.
VERSION ?= 0.7.0
DEB_REVISION ?= 1
DEB_ARCH ?= $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
MAINTAINER ?= Notepad++ Linux <notepad-plus-plus-linux@localhost>
PACKAGE := notepad-plus-plus-linux
DEB_ROOT := $(BUILD_DIR)/deb/root
DEB_WORK := $(BUILD_DIR)/deb/work
DEB_FILE := $(BIN_DIR)/$(PACKAGE)_$(VERSION)-$(DEB_REVISION)_$(DEB_ARCH).deb
DEB_FALLBACK_DEPENDS := libgtk-3-0 (>= 3.22), libglib2.0-0 (>= 2.56), libstdc++6, libc6

.PHONY: deb
deb: $(TARGET) $(SAMPLE_PLUGIN) | $(BIN_DIR)
	@command -v dpkg-deb >/dev/null || { echo "Instale o dpkg (sudo apt install dpkg-dev)"; exit 1; }
	rm -rf $(BUILD_DIR)/deb
	$(MAKE) install DESTDIR="$(abspath $(DEB_ROOT))" PREFIX=/usr PLUGIN_DIR=/usr/lib/notepad-plus-plus-linux/plugins
	install -Dm644 packaging/deb/copyright $(DEB_ROOT)/usr/share/doc/$(PACKAGE)/copyright
	install -Dm644 ../LICENSE $(DEB_ROOT)/usr/share/doc/$(PACKAGE)/LICENSE
	install -Dm644 README.md $(DEB_ROOT)/usr/share/doc/$(PACKAGE)/README.md
	strip --strip-unneeded $(DEB_ROOT)/usr/bin/notepad-plus-plus-linux $(DEB_ROOT)/usr/lib/notepad-plus-plus-linux/plugins/text-tools/text-tools.so
	mkdir -p $(DEB_WORK)/debian $(DEB_ROOT)/DEBIAN
	printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' $(PACKAGE) $(PACKAGE) > $(DEB_WORK)/debian/control
	cd $(DEB_ROOT) && find usr -type f -exec md5sum {} + > DEBIAN/md5sums
	depends="$$(cd $(DEB_WORK) && dpkg-shlibdeps -O "$(abspath $(DEB_ROOT))/usr/bin/notepad-plus-plus-linux" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"; \
	if [ -z "$$depends" ]; then depends='$(DEB_FALLBACK_DEPENDS)'; echo "Aviso: dpkg-shlibdeps indisponível; usando dependências genéricas."; fi; \
	size="$$(du -sk $(DEB_ROOT)/usr | cut -f1)"; \
	sed -e 's|@VERSION@|$(VERSION)-$(DEB_REVISION)|' -e 's|@ARCH@|$(DEB_ARCH)|' -e 's|@MAINTAINER@|$(MAINTAINER)|' \
		-e "s|@DEPENDS@|$$depends|" -e "s|@SIZE@|$$size|" packaging/deb/control.in > $(DEB_ROOT)/DEBIAN/control
	dpkg-deb --root-owner-group --build $(DEB_ROOT) $(DEB_FILE)
	@echo "Pacote criado: $(DEB_FILE)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/notepad-plus-plus-linux
	rm -f $(DESTDIR)$(PREFIX)/share/applications/notepad-plus-plus-linux.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/notepad-plus-plus-linux.svg
	rm -f $(DESTDIR)$(PREFIX)/share/metainfo/notepad-plus-plus-linux.metainfo.xml
	rm -f $(DESTDIR)$(PLUGIN_DIR)/text-tools/text-tools.so
	rm -f $(DESTDIR)$(PREFIX)/include/npp-linux-plugin.h

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)
