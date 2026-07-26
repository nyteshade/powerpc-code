---
name: release
description: Cut a versioned ppcode release - bump SemVer, build on the G5, verify, package the self-contained .app and the CLI, tag, and publish a GitHub release. Use when asked to release, tag, ship, publish a version, or produce downloadable builds.
---

# Releasing ppcode

Releases are SemVer, tagged `vX.Y.Z`, and published to
`github.com/nyteshade/powerpc-code` with two artifacts.

## The one command

```sh
./scripts/release.sh 0.3.0          # or --current to release VERSION as-is
./scripts/release.sh 0.3.0 --dry    # build and package, publish nothing
```

Run it from the Linux box. It syncs to the G5, builds there, and publishes from
here. Always do a `--dry` first if anything about the build has changed.

## What it does, and what it refuses to do

1. Rejects a non-SemVer version, a dirty tree, or an existing tag.
2. Writes `VERSION` and commits `Release vX.Y.Z`.
3. Clean-builds `ppcode`, `ppcode-gui` and `ppcode.app` on the G5.
4. **Gates.** It will not tag or publish unless all of these pass:
   - `./build/ppcode --selftest`
   - `./build/ppcode-gui --check`
   - `otool -L` shows no `/opt/local` in *any* of the three shipped binaries:
     the application, the tool inside it, and the standalone tool
   - the standalone tool actually runs, which linking correctly does not by
     itself prove
5. Packages, tags, pushes, and creates the GitHub release with notes.

The third gate is the important one — see below.

## Versioning

`VERSION` at the root of the tree is the only place the number lives. The
Makefile reads it and passes `-DPPCODE_VERSION`, so `--version`, the bundle's
`CFBundleShortVersionString` and the git tag cannot drift apart. Never edit a
version number anywhere else.

`deploy.sh` writes `.build-rev` from git on this box — the G5 copy has no `.git`
— so a development build reports `0.2.0+g1a2b3c4` while a release reports the
plain version.

Choosing the number: patch for fixes, minor for new capability, major only on a
deliberate break. Everything is pre-1.0, so minor is the usual bump.

## The two artifacts

| Artifact | Prerequisites |
| --- | --- |
| `ppcode-<v>-ppc-macos10.5-app.tar.gz` | **none** |
| `ppcode-<v>-ppc-macos10.5-cli.tar.gz` | **none** |

The binaries link four MacPorts dylibs directly, and libcurl drags in a further
thirteen for TLS, HTTP/2, compression and IDN — **seventeen in total**. Anything
shipped without them dyld-errors on a machine that has never seen MacPorts, so
`scripts/bundle_dylibs.sh` walks the dependency closure, copies it in beside the
executable, and rewrites the install names to a path relative to the executable:

- `--app  build/ppcode.app` → `@executable_path/../Frameworks`, covering
  `Contents/MacOS/ppcode` **and** `Contents/Resources/ppcode`.
- `--tree build/ppcode-cli` → `@executable_path/../lib`, for `bin/ppcode`.

### Why the CLI is linked, never copied

`bin/ppcode` finds its libraries at `../lib` relative to itself, so copying the
bare executable elsewhere strands it. Both installers therefore **symlink**:
`install.sh` in the tarball, and the Settings window for the copy inside the
bundle.

That works because Leopard's dyld resolves `@executable_path` *through* a
symlink — verified with `DYLD_PRINT_LIBRARIES=1`, which shows the bundled copies
being loaded through the link. It also means the installed tool is updated
whenever the application is, and that replacing a link in use is safe where
overwriting a running executable is not.

If you change this, keep the link. A copy of the bare executable cannot work.

## MacPorts prerequisites

Only for building from source — nothing shipped needs them at runtime:

```sh
./scripts/macports_prereqs.sh          # list, and check what is installed
./scripts/macports_prereqs.sh --pkg    # build double-click .mpkg installers
```

`port mpkg` packages what is already built, but will compile a missing
dependency from source first — hours to days on a G5. The script checks and
refuses rather than starting one by accident. Never suggest `port install`
casually for the same reason.

## If a gate fails

Fix the cause; do not bypass the gate. Nothing is tagged or pushed until every
gate passes, so a failed run leaves only a local `Release vX.Y.Z` commit — reset
it with `git reset --hard HEAD~1` if you are abandoning the attempt.
