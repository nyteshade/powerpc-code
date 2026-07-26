---
title: Aqua and Cocoa — the UX you are building for
priority: 30
min_context: 60000
tags: [aqua, cocoa, ui, hig]
---

## Aqua and Cocoa

The interface you are writing for is **Aqua** on Leopard, driven by **Cocoa**. This
is worth understanding rather than treating as a legacy constraint, because Cocoa
in 2007 was — and largely still is — one of the best-designed application
frameworks ever shipped, and code that works *with* it is dramatically shorter than
code that fights it.

### Why Cocoa rewards working with the grain

- **The object graph is the application.** A `.xib` is a serialised graph of real
  objects with their connections, not a description to be interpreted at runtime.
  Wiring an outlet or an action in Interface Builder removes code rather than
  adding it. Reach for the nib before reaching for `addSubview:`.
- **Target/action** decouples controls from behaviour: a button holds a target and
  a selector, so one method serves a menu item, a toolbar button, and a keyboard
  shortcut with no glue.
- **Key-value coding and key-value observing** let views bind directly to model
  properties. **Cocoa Bindings** on 10.5 can drive an entire inspector panel with
  zero controller code — set the binding in Interface Builder and the value,
  enabled state, and validation all follow.
- **The responder chain** means you rarely dispatch events manually. Implement
  `-copy:`, `-paste:`, `-selectAll:` on the object that owns the data and the menu
  items light up on their own, including their enabled state via
  `-validateUserInterfaceItem:`.
- **`NSDocument`** gives you the whole document lifecycle — open, save, save-as,
  revert, autosave, window titles, the dirty dot, and the "do you want to save"
  sheet — from two or three overridden methods.
- **Free behaviour you should not reimplement:** undo via `NSUndoManager`, text
  editing with spell-check and find, printing with `NSPrintOperation`, drag and
  drop, services, AppleScript support, `NSUserDefaults`, localisation via
  `.strings` and `.lproj`, and full keyboard access.
- **Cocoa's naming is documentation.** `tableView:objectValueForTableColumn:row:`
  reads as a sentence. Follow the convention exactly — verb-first for actions, no
  `get` prefix, `is`/`has` for booleans, delegate methods that begin with the
  sender — and other code (and other people) will predict your API correctly.

### Aqua design conventions that matter on 10.5

- **The menu bar belongs to the application, not the window.** Every command lives
  in a menu, with a keyboard equivalent; toolbar buttons and contextual menus are
  shortcuts to menu commands, never the only route to a feature.
- **Sheets, not modal dialogs**, for anything scoped to a window
  (`beginSheet:modalForWindow:...`). Application-modal alerts are for genuinely
  application-wide conditions.
- **Panels are utility windows** — inspectors, palettes — and use
  `NSPanel` with the utility style so they float and use a narrower title bar.
- **Preferences** live in a `Preferences…` item under the application menu, bound
  to `NSUserDefaults`, applied immediately with no OK button.
- **Layout**: Aqua has real metrics. 20px window margins, 8px between related
  controls, 12px between groups; right-align labels to a colon; align control
  baselines, not their frames. Interface Builder's guides encode all of this —
  follow them.
- **Capitalisation**: title case for menu items, buttons, and window titles;
  sentence case for labels, checkboxes, and help text. An ellipsis on a menu item
  (`Save As…`) means "this opens something for more input".
- **Buttons**: the default button is rightmost and pulses; Cancel is to its left.
  Verb labels (`Save`, `Delete`, `Don't Save`) beat `OK`/`Yes`/`No`.
- **Toolbars** are user-configurable via `NSToolbar` with item identifiers — let
  the user customise rather than hard-coding a fixed row.
- **Resolution**: assume 72dpi and non-Retina. `NSImage` with named images from
  the bundle; `.icns` for the app icon, with all sizes present.
- **The window's autosave name** (`setFrameAutosaveName:`) makes position and size
  persist for free.

### The visual language is skeuomorphic — lean into it

Nobody called it "skeuomorphism" in 2007, but that is exactly what Aqua was, and it
is the correct aesthetic for this platform. **Never propose flat design here.** Flat
UI arrived with iOS 7 in 2013 and looks broken and cheap next to Leopard's chrome.
No hairline borders on white, no borderless text buttons, no monochrome glyph
icons, no "minimal" whitespace-only hierarchy.

What the era actually looks like, and what you should reach for:

- **Depth is the organising principle.** Controls sit *on* surfaces. Everything has
  a light source from above: a subtle top-down gradient, a 1px light highlight on
  the top edge, a darker bottom edge, and a soft drop shadow beneath.
- **Materials, not colours.** Aqua surfaces read as physical: **brushed metal**
  window backgrounds, **unified titlebar-and-toolbar** gradients, glass, felt,
  linen, leather, torn paper, wood. Leopard specifically moved to the dark
  "unified" gradient titlebar and the glossy 3D Dock with a reflective shelf.
- **Glossy gel buttons.** The signature Aqua control is a rounded capsule with a
  bright specular highlight across its upper half and a soft inner shadow below.
  The default button pulses blue. Recessed controls (search fields, text fields)
  get an inner shadow so they look pressed *into* the surface.
- **Icons are detailed, photorealistic, and perspectival.** 512px, rendered
  objects at a slight angle with reflections and shadows — a real hard disk, a real
  folder, a real stamp. Not a flat pictogram. `.icns` with every size present, and
  the small sizes hand-tuned rather than naively downscaled.
- **Textures and pinstripes** are period-correct for panel backgrounds; scrollbar
  tracks and window backgrounds are subtly textured, not pure `#FFFFFF`.
- **Reflections and shine** are used liberally: the Dock reflects, Cover Flow
  reflects, album art reflects. Time Machine's starfield and Front Row's blur are
  the era's idea of delight.
- **Sheets slide out from under the titlebar** with a genuine animation; drawers
  slide out from window edges. These motions are part of the visual identity, not
  decoration to be optimised away.
- **Progress indicators** are the barber-pole striped bar and the spinning
  aqua-blue gear, not a thin flat line.

When you generate `.xib` layouts, CSS for an embedded `WebView`, or drawing code,
match this: gradients over solids, bevels and inner shadows over flat fills,
rounded rects with real highlights, detailed iconography. Use the standard
`NSBox`/`NSTabView`/`NSButton` bezel styles rather than drawing custom flat
controls — the stock controls already carry the correct look, and a hand-drawn flat
replacement will be both more work and visibly wrong.

If a user asks for something "modern looking", clarify: on this platform the
native, correct, and best-looking result is high-gloss Aqua, and a flat redesign
would fight both the framework and the surrounding OS.

### Practical guidance for generated code

- Controllers subclass `NSObject` and are instantiated *in the nib* with outlets
  connected; do not build UI in code unless it is genuinely dynamic.
- Use `NSArrayController` with a table view before writing a data-source by hand.
- `awakeFromNib` is where nib-loaded setup goes, not `init`.
- Retain/release rules: you own what you `alloc`/`new`/`copy`/`retain`, and must
  balance it. Outlets in a nib are owned by the nib's top-level objects — do not
  release them in `dealloc` unless you retained them yourself.
- Prefer `NSNotificationCenter` for one-to-many, delegates for one-to-one.
- Keep the model in Foundation types (`NSString`, `NSNumber`, `NSArray`,
  `NSDictionary`) so bindings, archiving, and property lists all work without
  conversion layers.
- Set `NSApplicationMain` in `main.m` and let the framework run the loop.

The short version: on this platform the framework is more capable than the
surrounding tooling, so lean on it hard. If a feature feels like it needs a lot of
code, check whether Cocoa already provides it — it usually does.
