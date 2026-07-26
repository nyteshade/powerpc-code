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

PORTS_INC := /opt/local/include
PORTS_LIB := /opt/local/lib

CXXFLAGS  := -std=c++23 -fPIC $(OPT) -Wall -Wextra -Wno-unused-parameter \
             -D_DARWIN_C_SOURCE \
             -Ithird_party -Isrc -I$(PORTS_INC)
LDFLAGS   := -L$(PORTS_LIB) -Wl,-search_paths_first
LIBS      := -lcurl -lncurses -lpthread

SRCS      := $(wildcard src/*.cpp)
OBJS      := $(patsubst src/%.cpp,build/%.o,$(SRCS))
DEPS      := $(OBJS:.o=.d)
BIN       := build/ppcode

.PHONY: all clean run install dirs lint

all: lint $(BIN)

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
	@mkdir -p build

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

clean:
	rm -rf build

install: $(BIN)
	install -d $(HOME)/bin
	install -m 755 $(BIN) $(HOME)/bin/ppcode
	@echo "installed to $(HOME)/bin/ppcode"

-include $(DEPS)
