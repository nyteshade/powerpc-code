---
title: Objective-C++ with MacPorts GCC on Leopard
priority: 60
min_context: 100000
tags: [objcpp, gcc, cocoa, gui]
---

## Writing Objective-C++ here

Mixing Cocoa with modern C++ works on this machine — `g++-mp-15 -std=c++23
-x objective-c++ ... -framework Cocoa` compiles and links, and an Objective-C
class can hold a `std::unique_ptr` to a C++ object with `std::ranges` and
`std::format` in the same translation unit. That is how a native GUI can be put
in front of a C++ engine here.

But GCC's Objective-C front end is not clang's, and it is missing things you will
reach for by reflex. These are the ones that actually stop a build:

### Fast enumeration does not exist

```objc
for (NSString *s in array) { ... }        // does NOT compile under GCC ObjC++
```

Use an enumerator instead:

```objc
NSEnumerator *e = [array objectEnumerator];
NSString *s;
while ((s = [e nextObject]) != nil) { ... }
```

### A C++ catch clause crashes the compiler

Any explicit `try`/`catch` in an Objective-C++ translation unit triggers

```
internal compiler error: in objc_eh_runtime_type,
at objc/objc-next-runtime-abi-01.cc:2798
```

This is not limited to catches inside a method — a free function in the same
`.mm` does it too. Implicit cleanups from `std::string`, `std::vector` and
friends are fine; it is emitting the *catch type* through the NeXT runtime path
that breaks.

Two ways round it:

- Use non-throwing APIs. `nlohmann::json::parse(text, nullptr, false)` returns a
  discarded value instead of throwing, so no catch is needed.
- Put anything that genuinely must catch into a plain `.cpp` compiled as C++,
  and call it from the `.mm`.

### Instance variables must be declared in the `@interface`

GCC targets the **fragile** Objective-C ABI on this platform, so declaring ivars
in the `@implementation` block is rejected:

```objc
@implementation MyClass {          // non-fragile-ABI syntax: not available
    int counter;
}
```

Declare them in the `@interface` braces instead. For the same reason, prefer
writing accessors by hand over relying on `@property`/`@synthesize` to
synthesise a backing ivar.

A C++ type can be forward-declared in an Objective-C header that is only ever
included from `.mm`, which keeps the engine out of the interface:

```objc
struct EngineState;                // C++ forward declaration

@interface Bridge : NSObject {
    struct EngineState *state;
}
```

### Non-ASCII inside an `@"..."` literal comes out as garbage

GCC compiles `@"..."` into an `NSConstantString` wrapping the literal's raw
bytes, and `NSConstantString` reads those bytes as ASCII. A UTF-8 ellipsis in a
literal therefore arrives as **three garbage characters**:

```objc
[item setTitle:@"Settings…"];        // renders as "Settings" + 3 junk glyphs
```

GCC does warn — `warning: non-ASCII character in CFString literal` — but it is
easy to miss in a long build, and the damage is cosmetic enough to ship. Build
anything non-ASCII at runtime from an explicitly UTF-8 C string instead:

```objc
NSString *PPUTF8(const char *utf8) { return [NSString stringWithUTF8String:utf8]; }

[item setTitle:PPUTF8("Settings\xE2\x80\xA6")];
```

`ppcode-gui --check` asserts that every menu title survives a UTF-8 round trip,
so a regression is caught rather than merely warned about. Note also that this
platform predates any emoji font: even a correctly built emoji string draws as a
hollow box, so use words.

### A nil-target menu item is silently disabled

An `NSMenuItem` with a nil target has its action resolved through
`-targetForAction:`. For the application's *own* actions that search can come up
empty, and AppKit then disables the item — and **a disabled item ignores its key
equivalent**. The symptom is a menu command that does nothing and a keyboard
shortcut that does nothing, while calling the same method directly works fine.

Give application actions an explicit `setTarget:`. Leave the standard AppKit
ones (`hide:`, `terminate:`, `cut:`, `copy:`) with a nil target on purpose, so
they travel the responder chain to whatever is first responder.

### Delegate protocols are informal on 10.5

`NSTableViewDataSource`, `NSTableViewDelegate`, `NSTextViewDelegate` and most
others were **informal** protocols until 10.6. Declaring conformance fails with
"cannot find protocol declaration". Just implement the methods; the framework
finds them by selector.

### Other 10.6-and-later things that will not compile

`-[NSApplication setActivationPolicy:]`, `NSAppearance`, `@autoreleasepool`,
`NS_ENUM`, object literals and subscripting, `instancetype`, GCD. See the core
Leopard document for the full list.

### Threading a C++ engine behind AppKit

AppKit is not thread-safe and drawing from a worker thread produces corruption
that surfaces as unrelated crashes much later. Run the engine on a
`std::thread` and marshal every callback back with

```objc
[target performSelectorOnMainThread:@selector(update:)
                         withObject:obj
                      waitUntilDone:NO];
```

For the reverse direction — the worker needs an answer from the user, such as a
permission prompt — block the worker on a `std::condition_variable` and have the
main-thread handler set the result and notify. Do not spin, and do not run a
modal loop from the worker.

### Verifying a GUI on a headless or sleeping machine

`screencapture` is useless from an SSH session. It returns a solid black frame —
mean 0, one colour, no error — and it does so **whether or not the display is
awake**, because the SSH session cannot read the console framebuffer. Measured on
this machine with the user watching the application on screen at the time.
`launchctl bsexec` into the console session needs root and fails with
`task_for_pid() (os/kern) failure`.

Two things do work:

1. **`--check`**: build the window, walk the view hierarchy, print what was
   found. Verifies construction and wiring with no display at all.
2. **`--shot <dir>`**: have the application screenshot *itself*.
   `-cacheDisplayInRect:toBitmapImageRep:` draws a view hierarchy into an
   offscreen bitmap, so it works with the display asleep, needs no root, and
   never touches the accessibility API — which is disabled by default and is a
   system-wide setting you should not enable on someone's machine just to test
   your own work.

```objc
NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:[view bounds]];
[view cacheDisplayInRect:[view bounds] toBitmapImageRep:rep];
[[rep representationUsingType:NSPNGFileType properties:nil] writeToFile:path
                                                            atomically:YES];
```

Worth capturing a scrolling view at its full laid-out height as well as at window
size — `usedRectForTextContainer:` gives the height — because a rendering bug is
just as likely to be in the part scrolled out of view.

### Laying out a pane in code: rectangles grow upward

`NSMakeRect(x, y, w, h)` puts the view's **bottom** edge at `y`, so a view
occupies `y .. y + h` and grows *up* the window. A top-down layout must move the
cursor down by a row's full height *before* placing anything in it. Decrement by
less than the height you then draw, and the new row grows back up through the row
above — which is what put every description on top of its text field in the
settings window. Route it through a helper so the mistake cannot recur:

```objc
static CGFloat NextRow(CGFloat *y, CGFloat height, CGFloat gap) {
  *y -= (height + gap);
  return *y;
}
```

Separately, an `NSTextField` does **not** wrap by default — a long note is laid
out as a single line and simply runs off the right edge of the pane.
`[[field cell] setWraps:YES]`.

### AppKit does not decorate trailing whitespace

A horizontal rule drawn as an underline over a run of spaces renders as nothing:
underline and strikethrough are skipped on trailing whitespace. A run of U+2500
depends on the system font having that glyph. Repeated em dashes (U+2014) are in
every font and butt together into a continuous line.
