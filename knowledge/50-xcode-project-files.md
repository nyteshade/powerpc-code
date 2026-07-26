---
title: The Xcode 3 project file format
priority: 50
min_context: 100000
tags: [xcode, pbxproj, plist]
---

## project.pbxproj on Xcode 3

**The format is an old-style NeXT/OpenStep ASCII property list.** Not XML, not
binary — plain text you can read and edit. This matters: it means a project can be
modified reliably by a tool, which is why `xcode_info`, `xcode_add_file`,
`xcode_set_setting` and `xcode_add_framework` exist. Prefer those tools over
hand-editing; they keep the cross-references consistent and write a backup.

For reference, the other formats in a project of this era:

| File | Format |
| --- | --- |
| `project.pbxproj` | old-style ASCII plist |
| `Info.plist` | XML plist (`plutil -convert` handles it) |
| `.xib` | XML |
| `.nib` (compiled) | binary |
| `*.pbxuser`, `*.perspectivev3` | old-style ASCII plist, per-user, safe to delete |

### Structure

```
// !$*UTF8*$!
{
    archiveVersion = 1;
    objectVersion = 45;          /* Xcode 3.1 */
    rootObject = 29B97313... /* Project object */;
    objects = {
        29B97313... /* Project object */ = {
            isa = PBXProject;
            mainGroup = ...;
            targets = ( 8D110726... /* Sample */, );
            buildConfigurationList = ...;
            compatibilityVersion = "Xcode 3.1";
        };
        ...
    };
}
```

Everything lives in the flat `objects` dictionary, keyed by a **24-character
uppercase hexadecimal id**, and referenced by that id from elsewhere. The
`/* ... */` comments are annotations Xcode writes for readability; they are not
semantically meaningful but keeping them makes the file diffable.

The object types you will actually touch:

- **`PBXProject`** — the root. Holds `targets`, `mainGroup`, and a project-level
  `buildConfigurationList`.
- **`PBXNativeTarget`** — one buildable product. Holds `name`, `productType`
  (e.g. `com.apple.product-type.application`), `buildPhases`, and its own
  `buildConfigurationList`.
- **`XCConfigurationList`** → **`XCBuildConfiguration`** — one per configuration
  (`Debug`, `Release`), each with a `buildSettings` dictionary. Target settings
  override project settings.
- **`PBXFileReference`** — a file on disk: `path`, optional `name`,
  `lastKnownFileType` (`sourcecode.c.objc`, `file.xib`, `wrapper.framework`, …),
  and `sourceTree` (`"<group>"` for project-relative, `"<absolute>"` for
  absolute, `SOURCE_ROOT`, `BUILT_PRODUCTS_DIR`).
- **`PBXGroup`** — a navigator folder; `children` is a list of file-reference or
  group ids.
- **`PBXBuildFile`** — the join between a file reference and a build phase. It has
  a `fileRef` and, for a file to be compiled, must be listed in a phase.
- **`PBXSourcesBuildPhase`**, **`PBXResourcesBuildPhase`**,
  **`PBXFrameworksBuildPhase`**, **`PBXShellScriptBuildPhase`** — each has a
  `files` array of `PBXBuildFile` ids.

### Adding a file correctly requires three edits

This is the part that is easy to get wrong. A file only compiles if **all** of
these exist:

1. a `PBXFileReference` in `objects`;
2. that reference's id in some `PBXGroup`'s `children` (otherwise it is invisible
   in the navigator);
3. a `PBXBuildFile` pointing at it, **and** that build file's id in the target's
   `PBXSourcesBuildPhase.files`.

Headers get steps 1 and 2 only — never add a `.h` to the sources phase.
Resources (`.xib`, `.strings`, images) go into `PBXResourcesBuildPhase` instead.
Frameworks go into `PBXFrameworksBuildPhase` with `sourceTree = "<absolute>"` and
a path under `/System/Library/Frameworks`.

A project with a dangling reference generally still opens, and then misbehaves in
confusing ways, so verify after editing: run `xcodebuild` and check the file
actually compiled.

### Build settings worth knowing on this platform

`ARCHS` (often `$(ARCHS_STANDARD_32_BIT)`), `SDKROOT` (`macosx10.5`),
`MACOSX_DEPLOYMENT_TARGET`, `GCC_VERSION`, `GCC_MODEL_TUNING` (`G5`),
`GCC_OPTIMIZATION_LEVEL`, `GCC_PREFIX_HEADER` and
`GCC_PRECOMPILE_PREFIX_HEADER`, `INFOPLIST_FILE`, `PRODUCT_NAME`,
`INSTALL_PATH`, `OTHER_CFLAGS`, `OTHER_LDFLAGS`, `HEADER_SEARCH_PATHS`,
`LIBRARY_SEARCH_PATHS`, `FRAMEWORK_SEARCH_PATHS`, `PREBINDING`,
`ALWAYS_SEARCH_USER_PATHS`, `DEBUG_INFORMATION_FORMAT`.

To build against MacPorts, set `HEADER_SEARCH_PATHS` to `/opt/local/include` and
`LIBRARY_SEARCH_PATHS` to `/opt/local/lib`, and add `-Wl,-search_paths_first` to
`OTHER_LDFLAGS` so the linker prefers those over stale copies in `/usr/lib`.

To use a MacPorts compiler instead of the Xcode one, set `CC` and `CXX` in the
build settings — but note that Xcode 3 also honours `GCC_VERSION`, and mixing the
two is a common source of confusion.

### Command line

```sh
xcodebuild -list                                  # targets and configurations
xcodebuild -configuration Debug                   # build
xcodebuild -configuration Debug clean
xcodebuild -target Sample -configuration Release
```

There are no schemes and no `xcworkspace` in this era; `-target` and
`-configuration` are the whole interface.
