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
   - `otool -L` on the bundled executable shows no `/opt/local` paths
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

## The two artifacts, and why they differ

| Artifact | Prerequisites |
| --- | --- |
| `ppcode-<v>-ppc-macos10.5-app.tar.gz` | **none** |
| `ppcode-<v>-ppc-macos10.5-cli.tar.gz` | MacPorts `curl`, `ncurses`, `gcc15` |

The binaries link four MacPorts dylibs directly, and libcurl drags in a further
thirteen for TLS, HTTP/2, compression and IDN — **seventeen in total**. A bundle
that just copies the executable dyld-errors on any machine without MacPorts, so
`gmake app` runs `scripts/bundle_dylibs.sh`, which walks the dependency closure,
copies everything under `/opt/local` into `Contents/Frameworks`, and rewrites the
install names to `@executable_path/../Frameworks`.

`Contents/Resources/ppcode` — the CLI copy that the Settings window installs into
`~/bin` — is **deliberately left alone**. Once it leaves the bundle
`@executable_path` points somewhere else, so rewriting it would break the one
thing it exists for. That is why the CLI tarball still needs MacPorts.

Do not "fix" this by rewriting the Resources copy.

## MacPorts prerequisites

For people building from source or running the bare CLI:

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
