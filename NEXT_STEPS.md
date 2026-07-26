# NEXT_STEPS

Handoff notes. Read this first after a restart; it is the state of play as of
the last commit on this branch.

---

## 1. Where things are

| | |
| --- | --- |
| **Authoritative source** | `/home/brie/projects/ppc-test` on the Linux box. All git history lives here. Add the GitHub remote here. |
| **Build/deploy target** | `/Users/brie/src/ppcode` on the PowerMac G5 (`brie@10.0.0.102`). A deployed copy only — **no git**, and `deploy.sh` uses `rsync --delete`, so anything edited there is destroyed on the next deploy. |
| **Brielle's own app** | `~/Desktop/Sample` on the G5 — her real Objective-C project (a pasteboard history app), built *using* ppcode. Never touched by deploy. |

The G5 is the only machine with the PowerPC toolchain, so **all builds happen
remotely**. Never try to build on the Linux box.

```sh
./deploy.sh              # rsync + gmake -j2
./deploy.sh -c           # clean build
./deploy.sh -r -p "hi"   # sync, build, run with args
```

Then on the G5:

```sh
gmake            # CLI          -> build/ppcode
gmake gui        # Cocoa app    -> build/ppcode-gui
gmake app        # .app bundle  -> build/ppcode.app  (also embeds the CLI)
./build/ppcode --selftest        # 413 offline checks, fast
./build/ppcode --selftest --net  # adds live OpenRouter calls
./build/ppcode-gui --check       # builds the window, walks the view tree, exits
python3 scripts/tui_drive.py ./build/ppcode -- 1.5 '/help\r' 1.5 '\x04'
```

`scripts/tui_drive.py` drives the TUI in a pty and renders the screen — the only
way to regression-test the interface without a human at the keyboard.

---

## 2. What exists now

**Three front ends over one engine.** `Agent` owns the model/tool loop and is
front-end agnostic; the TUI, the headless runner and the Cocoa app differ only in
the callbacks they install. Add features through `Agent::Events`, never in one
front end.

- **CLI/TUI** — ncurses, UTF-8, markdown + syntax highlighting, mouse scrolling,
  searchable model picker, mid-turn steering, sessions, compaction.
- **Headless** — `-p`, `--output json|stream-json`, job files (`-j`), `--continue`.
- **Cocoa app** — `ppcode gui`. Settings window, drag-and-drop attachments,
  session list, CLI installer.

**~30 tools**: files (incl. `multi_edit`, `read_many_files`), `bash`, `build`
(structured diagnostics), background jobs, web fetch/search, Apple docs search,
Xcode project (`.pbxproj`) tools, `.xib` tools, screenshots, app bundling and
dylib relocation, subagents (`task`, `task_batch`), todo.

**MCP** over stdio and HTTP, including Bearer-token auth (verified).

**Context system**: the machine is probed once and cached; a knowledge corpus in
`knowledge/*.md` is injected budget-aware, scaled to the model's context window.

---

## 3. Outstanding work, in priority order

### 3.1 GUI: native markdown rendering  *(task #39 — highest value)*

The transcript is plain `NSAttributedString` with three hardcoded styles. It
should render markdown properly: headings, emphasis, lists, quotes, inline code,
and fenced code blocks with syntax highlighting.

**Do not use a WebView.** Brielle was explicit: Leopard's WebKit is ancient and
JavaScript is slow on a G5. Native only.

**Reuse `render::highlight()`** from `src/render.cpp` for code blocks — it is
already written and tested and knows Objective-C. Do *not* reuse
`render::markdown()` for block layout: it wraps to a fixed column count for a
monospace terminal, which is wrong for a proportional font where the text view
does its own wrapping. Write GUI-appropriate block layout and map spans to
attributes.

Suggested home: `src/gui/Markdown.h/.mm`, exposing
`NSAttributedString *PPAttributedFromMarkdown(const std::string &md)`.

Code blocks want a Monaco font, a light background fill, and an indent —
`NSParagraphStyle` with `setFirstLineHeadIndent:`/`setHeadIndent:`.

### 3.2 GUI: wire the skeuomorphic skin through  *(task #40)*

`src/gui/Skin.mm` is written and compiles — procedural oxblood leather, aged
ruled paper, saddle stitching, recessed wells, raised panels, embossed text. It
is **not yet used** by `main.mm`.

To do: sidebar and window chrome in `PPLeatherView`, transcript backdrop in
`PPPaperView`, composer in a recessed well, an embossed title. Brielle asked for
"full skeuomorph" — this is the right direction for Leopard, and flat design is
explicitly wrong here (see `knowledge/30-aqua-and-cocoa.md`).

Textures are **generated, not downloaded** — deliberate: nothing to license or
ship, and a 128px tile computed once at launch is far cheaper on a G5 than
decompressing photographs. Keep it that way.

### 3.3 Reformat the Objective-C  *(task #41)*

`src/gui/*.mm` and `*.h` were written **before** Brielle's formatting guide
arrived, so they are 4-space indent with K&R braces. The guide is at
`~/OBJC_FORMATTING_guess.md` on the G5, summarised in
`knowledge/70-objc-style.md`.

Key rules: **2-space indent**, 90 columns, `NSString *name` star binding, blank
line before `return`, blank line between *every* type member, `else`/`@catch` on
a new line **after a blank line**, `case` at the same indent as `switch`,
message sends broken with the receiver alone on the first line.

`Settings.mm` helpers `Or()`/`OrObj()` are already in the new style; the rest is
not.

### 3.4 Terminal diagnostic for iTerm 2.0 Legacy

Brielle runs **iTerm 2.0 Legacy** on Leopard, not Apple's Terminal. Support both.
Add a `/term` command reporting `TERM`, what ncurses says about colours, whether
mouse reporting was accepted, and the locale/charset — so mismatches are visible
rather than guessed at.

### 3.5 Smaller queued items

- **`Client::set_config`** was added for the settings window; make sure a model
  change from the GUI reliably rebuilds the system prompt.
- The **GUI is untested against a live turn** — `--check` verifies construction
  only. It needs a real conversation run once a key is entered.
- **Attachment display** in the GUI is a plain label; a token row with remove
  buttons would be better.
- **`xib` connection editing** was deliberately not implemented — see §4.

---

## 4. Decisions already made — do not relitigate

- **No C++ iostreams anywhere.** libcurl drags in CoreServices and therefore a
  second libstdc++; Darwin coalesces weak symbols process-wide, so any iostream
  use frees across runtimes and floods stderr. `gmake lint` enforces this.
- **`.xib` connection editing is out of scope.** Declaring a class is safe and
  implemented; synthesising connection records is intricate and a subtly wrong
  nib opens and then misbehaves. The tool says so rather than guessing.
- **QuickJS is not required for MCP.** It only runs JavaScript servers locally.
  HTTP servers and other stdio executables need nothing extra.
- **Textures are procedural, not image assets.** See §3.2.
- **Approval uses typed letters + Enter, not single-key hotkeys.** Hotkeys were
  tried and removed: there is no way to tell `a` (approve all) from the `A`
  beginning "Actually, stop…", so the letter was swallowed and the tool silently
  approved. This was a real bug, not a preference.

---

## 5. Platform gotchas that will bite again

All documented in `knowledge/`, but the ones that cost the most time:

1. **GCC Objective-C++ has no blocks**, no fast enumeration (`for/in`), and
   **ICEs on any explicit C++ `catch` clause** (`objc_eh_runtime_type`). Use
   function pointers, `NSEnumerator`, and non-throwing APIs. It also cannot parse
   `x ?: @"literal"`. Fragile ABI, so ivars go in the `@interface`.
2. **ncurses `#define`s `hline`, `vline`, `border`** — never use those as local
   names; the error points somewhere unrelated.
3. **`size_t` underflow in padding maths** produces a multi-gigabyte string.
   Guard every `field - width`.
4. **A GUI app launched from the Finder inherits no shell environment**, which is
   why the API key must come from the config file or the Settings window.
5. **`plutil` cannot lint a `.xib`** — it fails on pristine files too. Verify with
   `ibtool --compile` instead.
6. **`port install` compiles from source** — hours to days here. Never suggest it
   casually.

---

## 6. Brielle's preferences

- Non-Anthropic/OpenAI frontier models, ascending capability and cost:
  `deepseek/deepseek-v4-pro`, `z-ai/glm-5.2`, `moonshotai/kimi-k3`. Pinned in the
  model picker so the ids never have to be recalled.
- Objective-C style: `~/OBJC_FORMATTING_guess.md`. A `SWIFT_FORMATTING.md` is
  coming; Swift cannot run on Leopard but generated Swift should still follow it.
- Skeuomorphic, era-correct visuals. Never propose flat design for this platform.
- Prefers being told when something is *not* needed (e.g. QuickJS) over being let
  run into a long build.

---

## 7. How to verify things properly

The display sleeps and the accessibility API is off, so screenshots often prove
nothing. Prefer:

- `--selftest` for anything with a parser or tool behaviour.
- `ppcode-gui --check` for interface construction.
- `tui_drive.py` for terminal rendering and key handling.
- `ibtool --compile` for nib validity; a clean `xcodebuild` for project validity.
- `otool -L` + a real run for dylib relocation.

Do not enable the accessibility API to test your own work — it is a system-wide
automation permission and not yours to flip.

---

## 8. Cost

Prompt caching is on by default and measured ~30% cheaper on a two-round task,
more on longer ones; hit rates of 99% are normal. `--max-cost` stops a run. Watch
`--show-context` if the system message grows.
