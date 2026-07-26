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

### ~~3.1 GUI: native markdown rendering~~ — **done**

Structure is parsed in `src/mdparse.cpp` (plain C++, so `--selftest` covers it —
38 checks); `src/gui/Markdown.mm` maps it to attributes. Headings, lists with
hanging indents, quotes, tables, rules, fenced code via `render::highlight()`,
and nesting emphasis. Streaming re-renders only closed blocks, via
`md::complete_prefix()`.

### ~~3.2 GUI: wire the skeuomorphic skin through~~ — **done**

Leather content view with stitching, gold embossed header, paper in recessed
wells. Two latent bugs in `Skin.mm` were fixed doing it: the noise lattice did
not wrap, so the "seamless" tiles were visibly gridded, and painting paper as a
pattern `NSColor` on a scrolling text view showed phase seams. Textures remain
procedural — keep it that way.

### 3.3 Reformat the Objective-C  *(task #41 — next)*

`src/gui/*.mm` and `*.h` were written **before** Brielle's formatting guide
arrived, so they are 4-space indent with K&R braces. The guide is at
`~/OBJC_FORMATTING_guess.md` on the G5, summarised in
`knowledge/70-objc-style.md`.

Key rules: **2-space indent**, 90 columns, `NSString *name` star binding, blank
line before `return`, blank line between *every* type member, `else`/`@catch` on
a new line **after a blank line**, `case` at the same indent as `switch`,
message sends broken with the receiver alone on the first line.

`Settings.mm` helpers `Or()`/`OrObj()` are already in the new style, as are
`Markdown.mm` and the parts of `Settings.mm`/`main.mm` touched since; the rest is
not.

### 3.4 Terminal diagnostic for iTerm 2.0 Legacy

Brielle runs **iTerm 2.0 Legacy** on Leopard, not Apple's Terminal. Support both.
Add a `/term` command reporting `TERM`, what ncurses says about colours, whether
mouse reporting was accepted, and the locale/charset — so mismatches are visible
rather than guessed at.

### 3.5 Smaller queued items

- ~~**`Client::set_config`** model-change rebuild~~ — done, and `--check` now
  asserts that changing the model both rebuilds the system prompt and names the
  new model in it. Config changes are also refused mid-turn: the worker thread
  reads `cfg` and the prompt for the whole of a turn, so changing them under it
  was a data race.
- ~~**Attachment display**~~ — done: a row of removable tokens above the
  composer, replacing a label that gave only a count.
- The **GUI has been run for real** (Brielle, 26 Jul): it launches, settings
  persist, a changed default model survives a relaunch. **Streaming markdown has
  still not been watched during a live turn** — that is the remaining unknown,
  and the only §3.5 item left. `--check` and `--shot` cover construction and
  rendering, but neither drives `-streamDelta:` with real deltas.
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

**`screencapture` over ssh is worthless.** It returns a uniformly black frame —
mean 0, one colour, no error — whether or not the display is awake, because the
ssh session cannot read the console framebuffer. Measured with Brielle watching
the app on screen at the time. `launchctl bsexec` into the console session needs
root and fails.

Prefer:

- `--selftest` for anything with a parser or tool behaviour.
- `ppcode-gui --check` for interface construction *and wiring* — it now also
  asserts menu items resolve to a target and that titles survive a UTF-8 round
  trip, both of which shipped broken once.
- **`ppcode-gui --shot <dir>`** for anything visual. The application screenshots
  itself with `-cacheDisplayInRect:toBitmapImageRep:`, which draws offscreen, so
  it works with the display asleep, needs no root, and never touches the
  accessibility API. Writes the window, the transcript at full laid-out height,
  and every settings tab. `scp` them back and look at them — this is what caught
  the double-spaced code blocks, the invisible rule, and the tile seams.
- `tui_drive.py` for terminal rendering and key handling.
- `ibtool --compile` for nib validity; a clean `xcodebuild` for project validity.
- `otool -L` + a real run for dylib relocation.

Do not enable the accessibility API to test your own work — it is a system-wide
automation permission and not yours to flip.

---

## 9. Releasing

`./scripts/release.sh 0.3.0` does the whole thing; `.claude/skills/release`
documents it. `VERSION` at the root is the only place the number lives.

**Nothing shipped needs MacPorts.** `gmake app` and `gmake cli-dist` both run
`scripts/bundle_dylibs.sh`, which copies the seventeen-library closure in beside
the executable and rewrites the install names relative to it. The release will
not publish unless all three shipped binaries are clean of `/opt/local` and the
standalone tool actually runs.

The CLI is always **symlinked**, never copied — it finds its libraries at a path
relative to itself, and Leopard's dyld resolves `@executable_path` through a
symlink. MacPorts is now only needed to build from source.

---

## 8. Cost

Prompt caching is on by default and measured ~30% cheaper on a two-round task,
more on longer ones; hit rates of 99% are normal. `--max-cost` stops a run. Watch
`--show-context` if the system message grows.
