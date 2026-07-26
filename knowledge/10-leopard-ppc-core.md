---
title: Writing code for Mac OS X 10.5 Leopard on PowerPC
priority: 10
tags: [leopard, ppc, core]
---

## Targeting Mac OS X 10.5 on PowerPC

You are generating code for **Mac OS X 10.5 (Leopard)** on **PowerPC**. This is a
2005-2007 platform. Most of what you know about modern macOS development does not
apply. The list below is the set of mistakes that actually waste build cycles here.

### Language and runtime availability

These do **not** exist on this system. Do not emit them:

- **ARC** (`__strong`, `__weak`, automatic retain/release). Leopard is
  manual retain/release only. Write `[obj retain]` / `[obj release]` /
  `[obj autorelease]` and balance them yourself.
- **`@autoreleasepool { }`** — that keyword arrived with clang on 10.7. Use
  `NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init]; ... [pool release];`
- **Grand Central Dispatch / `libdispatch`** (`dispatch_async`, `dispatch_queue_t`)
  — 10.6 and later. Use `NSThread`, `NSOperationQueue`, or pthreads.
- **Blocks in the system frameworks.** The `^{ }` syntax needs a compiler that
  supports it *and* a runtime. Apple's Leopard GCC does not support blocks at all.
  MacPorts clang plus `-fblocks -lBlocksRuntime` can compile them, but no Apple
  framework on 10.5 takes a block parameter, so blocks are only useful in your own
  code.
- **`NSArray`/`NSDictionary` literal syntax** (`@[...]`, `@{...}`) and boxed
  expressions (`@(x)`) — clang-era, not available. Use
  `[NSArray arrayWithObjects:..., nil]`.
- **Objective-C subscripting** (`arr[0]` on an `NSArray`) — not available.
- **`instancetype`** — use `id`.
- **`NSInteger`-era modern enums** (`NS_ENUM`) — not defined; use plain `enum`
  and `typedef`.
- **Modules / `@import`** — no.
- **Swift** — no.
- **libc++** — Leopard has only libstdc++. `std::unique_ptr`, `std::shared_ptr`,
  `auto`, lambdas and anything else C++11 require a MacPorts GCC, not the system
  compiler and not the old system libstdc++ headers.

These **do** exist on 10.5 and are safe to use:

- Objective-C 2.0: `@property` / `@synthesize`, fast enumeration
  (`for (id x in collection)`), `@optional` protocol methods.
- Garbage collection for Objective-C exists on 10.5 but is a dead end; do not use it.
- `NSOperation` / `NSOperationQueue`, `NSThread`, POSIX threads.
- Core Foundation, Core Graphics/Quartz, Core Animation, Core Data, WebKit.
- **Carbon**, still fully supported here, but 32-bit only. Prefer Cocoa for new work.
- `launchd` and `launchctl` for services.

### Big-endian: this is the pitfall that bites hardest

PowerPC is **big-endian**. Any code that treats a byte buffer as an integer, or
reads a binary file format, must swap explicitly.

- Use `CFSwapInt32LittleToHost`, `CFSwapInt32BigToHost`, `OSSwapInt32`, or
  `ntohl`/`htonl` — never a raw cast or `memcpy` into an `int`.
- Struct field order and padding differ from x86. Never write a `struct` straight
  to disk and expect another machine to read it.
- Anything reading PNG, WAV, ZIP, or a little-endian on-disk format needs swaps.
- Hash and checksum implementations that index bytes out of a word are a classic
  source of silent, platform-specific wrongness. Test them.
- `NSHostByteOrder()` returns `NS_BigEndian` here.

### Architecture and deployment flags

- Compile for the deployment target explicitly:
  `-mmacosx-version-min=10.5` and/or `export MACOSX_DEPLOYMENT_TARGET=10.5`.
- Against an SDK: `-isysroot /Developer/SDKs/MacOSX10.5.sdk`.
- Architecture: `-arch ppc` (32-bit) or `-arch ppc64`. The G5 can run 64-bit, but
  GUI applications on 10.5 are normally 32-bit; only go 64-bit if you need the
  address space, and know that some frameworks are 32-bit only.
- Universal binaries: build each arch, then `lipo -create a b -output universal`.
  Inspect with `lipo -info` and `file`.
- The CPU has **AltiVec/VMX**, not SSE. `-maltivec -mabi=altivec`, and check at
  runtime with `sysctl hw.optional.altivec`. Do not emit SSE intrinsics.
- Tune with `-mcpu=G5` (or `G4`); `GCC_MODEL_TUNING = G5` in Xcode.

### Linking and libraries

- Shared libraries are `.dylib`, not `.so`. Bundles are `.bundle`.
- Inspect with `otool -L` (not `ldd`) and `nm`. Change install names with
  `install_name_tool` (not `patchelf`).
- `@executable_path` and `@loader_path` work. `@rpath` exists on 10.5 but the
  stock linker's support is limited — with MacPorts `ld64` it is reliable.
- Link frameworks with `-framework Foundation -framework AppKit`, not `-l`.
- There is no SIP and no code signing requirement; `DYLD_LIBRARY_PATH`,
  `DYLD_INSERT_LIBRARIES` and `DYLD_PRINT_LIBRARIES` all work and are useful for
  debugging.
- Two-level namespace is the default, but Darwin coalesces **weak** symbols
  (template instantiations, `operator new`/`delete`, inline functions) across the
  whole process. Loading two copies of a C++ runtime therefore causes memory to be
  allocated by one and freed by the other. If you see
  `malloc: *** error for object 0x...: Non-aligned pointer being freed`, suspect
  exactly this before suspecting your own code.

### The userland is old BSD, not GNU

Commands here are the BSD/Leopard versions. These GNU-isms will fail:

- `sed -i` **requires** an argument on BSD: `sed -i '' -e ...`
- No `find -printf`, no `readlink -f`, no `grep -P`, no `grep -r --include`
  (use `find ... -exec grep`), no `cp --parents`, no `date -d`,
  no `stat -c` (BSD uses `stat -f`), no `tar --strip-components` on the old tar,
  no `mktemp --tmpdir`, no `seq -w` in older versions.
- `bash` is **3.2**: no associative arrays (`declare -A`), no `${var^^}`,
  no `mapfile`/`readarray`, no `**` globstar, no `[[ =~ ]]` BASH_REMATCH quirks
  of newer versions.
- If MacPorts `coreutils` is installed the GNU tools exist as `g`-prefixed names
  (`gsed`, `gfind`, `gstat`). Only use them if you have confirmed they are present.
- The filesystem is HFS+ and **case-insensitive** by default. `Foo.h` and `foo.h`
  are the same file. Do not rely on case to distinguish names.
- Resource forks and `.DS_Store` exist; `cp -R` may need `-p`, and `ditto`
  preserves metadata that `cp` does not.

### Xcode on this machine

- Xcode 3.x uses `.xcodeproj/project.pbxproj` with `objectVersion = 45` and
  `compatibilityVersion = "Xcode 3.1"`. Modern build settings, schemes,
  `xcworkspace`, asset catalogs and storyboards do not exist.
- Interface files are `.xib`/`.nib`, edited in Interface Builder.
- Build from the command line with `xcodebuild -configuration Debug`.
- Prefer editing `project.pbxproj` surgically; it is a plist-like format and a
  malformed edit makes the project unopenable.
