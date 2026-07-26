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

`screencapture` returns a solid black frame when the display is asleep, so a
screenshot proves nothing. Give the application a `--check` mode that builds its
window, walks the view hierarchy, and prints what it found. That verifies the
interface was constructed correctly without needing a visible display or the
accessibility API — which is disabled by default and is a system-wide setting
you should not enable on someone's machine just to test your own work.
