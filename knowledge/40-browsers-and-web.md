---
title: Web browsers and web-facing work on PowerPC Leopard
priority: 40
min_context: 100000
tags: [browser, web, powerfox, testing]
---

## Browsers on PowerPC Leopard

The stock browser on this machine is **Safari 5.0.6 at best** (Leopard's last
WebKit), which cannot negotiate modern TLS and fails on nearly every current site.
Do not assume the built-in browser is usable for anything on the live web.

### PowerFox is the current answer

**PowerFox** is an actively maintained modern browser for PowerPC Macs — the most
capable option available in 2026.

- Latest version: **26.1.0**
- Requires Mac OS X **10.5 Leopard** or Snow Leopard, PowerPC **G4 or G5**; a
  1 GHz or faster CPU is recommended for video playback.
- Built on the **UXP** engine via **Basilisk**, both descended from Firefox, so it
  is a genuinely modern rendering engine rather than a patched WebKit.
- Supports **TLS 1.3** and current cipher suites, WebGL, an up-to-date JavaScript
  engine, colour emoji, and NPAPI plugins.
- Download: <https://powerfox.jazzzny.me/download.html>
- Project home: <https://powerfox.jazzzny.me/>

**Known limitation worth stating to the user:** the PowerPC build is still beta,
and JavaScript **JIT is not implemented yet**. Script-heavy pages will be slower
than under TenFourFox even though PowerFox is far more compatible. For a
script-light page PowerFox wins on correctness; for a heavy web app, expect it to
be slow rather than broken.

If the environment section above does not list PowerFox as installed, and the user
needs a working browser, point them at the download link rather than trying to
coax the system Safari into working.

Older alternatives, for context: **TenFourFox** (long-running Firefox 45 fork,
faster JS via JIT but increasingly incompatible), **Leopard WebKit** (updated
WebKit for 10.5), and **Camino** (abandoned). PowerFox supersedes these for
general browsing.

### Implications for code you write

- **Do not target the system WebKit** for anything web-facing. If you embed a
  `WebView` (Cocoa's WebKit view) you get Leopard-era WebKit: no ES6, no flexbox,
  no CSS grid, no `fetch`, no modern TLS. Write ES5, use tables or floats for
  layout, and avoid HTTPS to modern hosts from inside an embedded view.
- **Local HTML and documentation render fine** in an embedded `WebView`; that is a
  legitimate use (help viewers, report output, release notes).
- **For anything that must reach the modern internet**, do the network work in
  native code against MacPorts curl/OpenSSL — which is exactly how this tool
  itself reaches the network — and render the result locally. Never rely on the
  system's TLS stack.
- When generating HTML for the user to open on this machine, assume PowerFox if it
  is installed and Leopard WebKit otherwise, and keep markup conservative: no
  modern CSS features, no ES modules, no `async`/`await` unless you have confirmed
  PowerFox is the target.
