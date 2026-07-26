---
title: MacPorts and building third-party software on vintage PowerPC
priority: 20
min_context: 60000
tags: [macports, build, ppc]
---

## Building third-party software here

### Installing a port is a build, not a download

There are no binary archives for this architecture. `port install` **compiles from
source**, on slow hardware. A large port can take many hours; a toolchain or a Qt
can take more than a day.

Before suggesting an install:

1. Check whether it is already there: `port -q installed NAME`, or look at the
   installed list in the environment section above.
2. Check the cost: `port deps NAME` and `port rdeps NAME` show what else would
   have to build first. A short dependency list is an hour; a long one is a weekend.
3. If it really is needed, say so explicitly, name the port, warn that it is a long
   build, and start it with the background job tool rather than a blocking command.
   Never trigger an install as an incidental step of some other task.

Useful, cheap commands: `port info NAME`, `port variants NAME`,
`port -q installed active`, `port provides /path/to/file`.

### Compiler selection

Modern MacPorts GCC is the workhorse: it produces working PowerPC code and
supports current C++ standards. Prefer it over the system compiler and over old
LLVM for anything non-trivial.

When configuring autotools projects, point them at the right compiler and paths
explicitly rather than trusting the defaults:

```sh
export CC=gcc-mp-15 CXX=g++-mp-15
export CPPFLAGS="-I/opt/local/include"
export LDFLAGS="-L/opt/local/lib -Wl,-search_paths_first"
export PKG_CONFIG_PATH=/opt/local/lib/pkgconfig
./configure --prefix=/opt/local ...
gmake -j2
```

`-Wl,-search_paths_first` matters: without it the old linker may prefer a stale
`/usr/lib` library over the MacPorts one you asked for.

Use `gmake`, not `/usr/bin/make`. Parallelism should match the core count; more
just thrashes.

### Common failure patterns in old-vs-new source

Modern source trees frequently assume things this platform lacks. Recognise these:

- **`configure` picks the wrong compiler** and then fails on C++11 syntax. Set
  `CC`/`CXX` before configuring, not after.
- **Missing POSIX functions.** Leopard has no `posix_memalign`, no
  `clock_gettime`, no `pthread_setname_np`, no `memmem`, no `strnlen` in some
  cases, no `openat`/`*at` family, no `utimensat`, no `arc4random_buf`.
  MacPorts `legacy-support` provides many of these — link
  `-lMacportsLegacySupport` — but note that its `posix_memalign` returns an
  offset pointer that plain `free()` cannot release.
- **`-std=` flags the system compiler rejects.** `cc1plus: error: unrecognized
  command line option "-std=c++17"` means the build fell back to Apple GCC 4.x.
- **`AC_` macros too old/new.** Regenerating with the local autotools sometimes
  fixes a stale `configure`; `autoreconf -fiv` is the usual incantation, but it
  can also make things worse — try building the shipped `configure` first.
- **CMake assuming a newer platform.** Pass
  `-DCMAKE_OSX_DEPLOYMENT_TARGET=10.5 -DCMAKE_OSX_ARCHITECTURES=ppc` and expect
  to have to patch feature checks.
- **Little-endian assumptions** in the source. Grep for `__LITTLE_ENDIAN__`,
  `htole`, `le32toh`, and any `union { int i; char c[4]; }` pattern.
- **Rust/Go/Node components** in a build. None of those toolchains exist for this
  platform. A project with a mandatory Rust or Node build step cannot be built
  here at all; say so rather than working around it indefinitely.

### Debugging

- `gdb` is available (Xcode 3 vintage). `lldb` is not.
- `gdb -batch -x cmds ./prog` with `break somewhere` / `run` / `bt` works well for
  non-interactive diagnosis.
- For allocator problems: `break malloc_error_break` then `bt` — Leopard's malloc
  calls it on every detected error, and the backtrace names the real culprit.
- `MallocStackLogging=1`, `MallocGuardEdges=1` and `MallocScribble=1` are
  available as environment variables.
- `sample`, `top`, `vm_stat`, `fs_usage` and `dtrace` exist (DTrace arrived in 10.5).
- `otool -tV` disassembles; PowerPC assembly, not x86.

### Relocating MacPorts libraries into a bundle

To ship something that runs on a machine without MacPorts, copy the dylibs into
the bundle and rewrite their paths:

```sh
cp /opt/local/lib/libfoo.1.dylib MyApp.app/Contents/Frameworks/
install_name_tool -id @loader_path/../Frameworks/libfoo.1.dylib \
    MyApp.app/Contents/Frameworks/libfoo.1.dylib
install_name_tool -change /opt/local/lib/libfoo.1.dylib \
    @loader_path/../Frameworks/libfoo.1.dylib MyApp.app/Contents/MacOS/MyApp
```

Then verify nothing still points into `/opt/local`:
`otool -L` every binary and grep for it. Transitive dependencies are easy to miss,
so check the dylibs against each other, not just the executable.
