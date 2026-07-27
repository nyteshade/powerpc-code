# ppcode -- a Claude Code style TUI for OpenRouter, built for PowerPC Leopard.
#
# Target host: PowerMac G5 / Mac OS X Server 10.5.8 / Darwin 9.8.0 ppc
# Toolchain:   MacPorts gcc15 (15.2.0), libcurl 8.21, ncurses 6.6
#
# gcc15 handles C++23 fine here; -fPIC is kept deliberately (see README: the
# clang 3.3 fallback on this box miscompiles without it, and it costs nothing).

# Note: ':=' not '?=' -- make has a built-in default for CXX, so '?=' would
# never fire and the build would silently fall back to the system g++-4.2,
# which has no idea what -std=c++23 means. An explicit 'gmake CXX=...' on the
# command line still overrides this.
CXX       := g++-mp-15
OPT       ?= -O1

# The single source of truth for the version. Everything -- --version, the
# bundle's Info.plist, the release tag -- reads it from here, so they cannot
# drift. Bump the VERSION file, or use scripts/release.sh.
VERSION   := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
# deploy.sh writes .build-rev on the machine with the git checkout; the G5 copy
# has no .git, so this is empty for a build made straight on the G5.
BUILD_REV := $(shell cat .build-rev 2>/dev/null)

PORTS_INC := /opt/local/include
PORTS_LIB := /opt/local/lib

CXXFLAGS  := -std=c++23 -fPIC $(OPT) -Wall -Wextra -Wno-unused-parameter \
             -D_DARWIN_C_SOURCE \
             -DPPCODE_VERSION='"$(VERSION)"' \
             -DPPCODE_BUILD_REV='"$(BUILD_REV)"' \
             -Ithird_party -Isrc -I$(PORTS_INC)
LDFLAGS   := -L$(PORTS_LIB) -Wl,-search_paths_first
LIBS      := -lcurl -lncurses -lpthread

SRCS      := $(wildcard src/*.cpp)
OBJS      := $(patsubst src/%.cpp,build/%.o,$(SRCS))
DEPS      := $(OBJS:.o=.d)
BIN       := build/ppcode

# The Cocoa front end. Objective-C++ so it can hold the C++ engine directly --
# verified working with gcc15 at C++23 against Leopard's AppKit.
GUI_SRCS  := $(wildcard src/gui/*.mm)
GUI_OBJS  := $(patsubst src/gui/%.mm,build/gui/%.o,$(GUI_SRCS))
GUI_DEPS  := $(GUI_OBJS:.o=.d)
# Everything except main.cpp: the GUI supplies its own entry point.
CORE_OBJS := $(filter-out build/main.o,$(OBJS))
GUI_BIN   := build/ppcode-gui
# The bundle is what the Finder shows, so it carries the human name. It has a
# space in it, which make cannot express as a target -- hence `app` being a
# recipe rather than a file rule, and every use of $(APP) being quoted.
APP_NAME  := PowerPC Code
APP       := build/$(APP_NAME).app

.PHONY: all clean run install dirs lint gui app cli-dist

all: lint $(BIN)

gui: lint $(GUI_BIN)


# This process ends up with two C++ runtimes (libcurl -> CoreServices ->
# /usr/lib/libstdc++.6.dylib, alongside gcc15's). Darwin coalesces weak symbols
# process-wide, so any iostream/locale object gets allocated by one runtime and
# freed by the other -- every construction prints "Non-aligned pointer being
# freed" to stderr. Using stdio instead avoids it entirely, so guard against
# anyone reintroducing the headers.
lint:
	@bad=$$(grep -ln '^[[:space:]]*#include[[:space:]]*<\(iostream\|fstream\|sstream\|iomanip\)>' \
	          src/*.cpp src/*.hpp 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
	  echo "ERROR: C++ iostreams are banned in this codebase (dual libstdc++)."; \
	  echo "       Use stdio / read_file_text / write_file_text instead."; \
	  echo "       Offending files:"; echo "$$bad" | sed 's/^/         /'; \
	  exit 1; \
	fi

dirs:
	@mkdir -p build build/gui

# Link to a temporary name and rename into place. Writing directly over a binary
# that is currently executing fails with ETXTBSY, and rebuilding while ppcode is
# running is a normal thing to do. rename(2) is atomic and leaves the running
# process on its old inode, so it keeps working until it exits.
$(BIN): $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) $(LIBS) -o $@.tmp
	@mv -f $@.tmp $@
	@echo "built $@"

build/%.o: src/%.cpp | dirs
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/gui/%.o: src/gui/%.mm | dirs
	$(CXX) $(CXXFLAGS) -x objective-c++ -MMD -MP -c $< -o $@

$(GUI_BIN): $(CORE_OBJS) $(GUI_OBJS)
	$(CXX) $(LDFLAGS) $(CORE_OBJS) $(GUI_OBJS) $(LIBS) -framework Cocoa -o $@.tmp
	@mv -f $@.tmp $@
	@echo "built $@"

# A real application bundle, so it can be double-clicked and appears in the Dock
# with a name rather than as a bare executable.
# The application carries the command line tool inside it, so the GUI can be the
# thing that installs and updates the CLI.
app: $(GUI_BIN) $(BIN)
	@rm -rf "$(APP)"
	@mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	@cp $(GUI_BIN) "$(APP)/Contents/MacOS/ppcode"
	@cp $(BIN) "$(APP)/Contents/Resources/ppcode"
	@cp resources/ppcode.icns "$(APP)/Contents/Resources/ppcode.icns"
	@printf '%s' 'APPL????' > "$(APP)/Contents/PkgInfo"
	@printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '  <key>CFBundleName</key><string>$(APP_NAME)</string>' \
	  '  <key>CFBundleDisplayName</key><string>$(APP_NAME)</string>' \
	  '  <key>CFBundleExecutable</key><string>ppcode</string>' \
	  '  <key>CFBundleIconFile</key><string>ppcode</string>' \
	  '  <key>CFBundleIdentifier</key><string>me.nyteshade.ppcode</string>' \
	  '  <key>CFBundleVersion</key><string>$(VERSION)$(if $(BUILD_REV),+g$(BUILD_REV),)</string>' \
	  '  <key>CFBundleShortVersionString</key><string>$(VERSION)</string>' \
	  '  <key>CFBundlePackageType</key><string>APPL</string>' \
	  '  <key>CFBundleSignature</key><string>????</string>' \
	  '  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>' \
	  '  <key>LSMinimumSystemVersion</key><string>10.5</string>' \
	  '  <key>NSPrincipalClass</key><string>NSApplication</string>' \
	  '  <key>NSHighResolutionCapable</key><false/>' \
	  '</dict></plist>' > "$(APP)/Contents/Info.plist"
	@bash scripts/bundle_dylibs.sh --app "$(APP)"
	@echo "built $(APP) ($(VERSION))"

# A self-contained command line distribution: bin/ppcode plus the MacPorts
# closure in lib/, linked with @executable_path/../lib so it runs from wherever
# it is untarred. This is what makes the CLI shippable to a machine that has
# never seen MacPorts.
CLIDIST := build/ppcode-cli

cli-dist: $(BIN)
	@rm -rf $(CLIDIST)
	@mkdir -p $(CLIDIST)/bin $(CLIDIST)/lib
	@cp $(BIN) $(CLIDIST)/bin/ppcode
	@bash scripts/bundle_dylibs.sh --tree $(CLIDIST)
	@cp scripts/cli_install.sh $(CLIDIST)/install.sh
	@chmod +x $(CLIDIST)/install.sh
	@printf '%s\n' \
	  'ppcode $(VERSION) -- PowerPC, Mac OS X 10.5' \
	  '' \
	  'Self-contained: bin/ppcode carries its own copies of libcurl, ncurses' \
	  'and the gcc15 runtime in lib/, so MacPorts is not required.' \
	  '' \
	  'Run it from here:      ./bin/ppcode --help' \
	  'Or put it on PATH:     ./install.sh [directory, default ~/bin]' \
	  '' \
	  'install.sh symlinks bin/ppcode rather than copying it, so keep this' \
	  'directory where it is. Delete the symlink to uninstall.' \
	  > $(CLIDIST)/README.txt
	@echo "built $(CLIDIST) ($(VERSION))"

clean:
	rm -rf build

install: $(BIN)
	install -d $(HOME)/bin
	install -m 755 $(BIN) $(HOME)/bin/ppcode
	@echo "installed to $(HOME)/bin/ppcode"

-include $(DEPS)
-include $(GUI_DEPS)
