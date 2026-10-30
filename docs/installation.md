# Installation and building

## Requirements

* macOS 13 or later (Apple Silicon or Intel)
* Xcode 15 or later (Command Line Tools are not enough for the app, because it needs SwiftUI and
  the macOS SDK)
* CMake 3.28 or later (`brew install cmake`) and, optionally, Ninja (`brew install ninja`)
* Network access on the first configure, to fetch Luau 0.744 from GitHub (or see *Offline builds*)

## Build the app with Xcode (recommended)

```bash
git clone https://github.com/AJJazzy/Exec.git Nexus
```

```bash
cd Nexus
```

```bash
scripts/build/xcode.sh Release
```

This generates `build/xcode/Nexus.xcodeproj` (a universal arm64 + x86_64 build), builds it and runs
the test suite. The app is written to `build/xcode/bin/Release/Nexus.app`.

To work in Xcode:

```bash
scripts/build/xcode.sh Debug --no-build
```

```bash
open build/xcode/Nexus.xcodeproj
```

Select the **Nexus** scheme and press ⌘R. For an Apple Silicon-only build, set `NEXUS_ARCHS=arm64`
before running the script.

## Build with Ninja (command line)

```bash
scripts/build/build.sh Release
```

This builds the core, `nexus-runner`, `nexus-cli`, the tests and, on macOS, `Nexus.app`, then runs
the tests. Output goes to `build/release/bin`.

## Install

Copy `Nexus.app` to `/Applications`. `nexus-runner` is embedded in `Nexus.app/Contents/MacOS`, so
nothing else needs to be installed. `nexus-cli` can be copied anywhere next to a `nexus-runner`
binary.

Nexus stores its data in `~/Library/Application Support/Nexus`. To uninstall, delete the app and
that folder.

## Packaging, code signing and notarization

```bash
scripts/package/package-macos.sh build/xcode/bin/Release/Nexus.app dist
```

This produces `dist/Nexus-<version>-macOS.zip`, a `.dmg` and `SHA256SUMS`.

* By default the app is **ad hoc** signed (`codesign -s -`) with the Hardened Runtime. Ad hoc builds
  run on the Mac that built them. On other Macs, Gatekeeper rejects downloaded ad hoc apps.
* To distribute, sign with a Developer ID Application certificate:

  ```bash
  NEXUS_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" scripts/package/package-macos.sh build/xcode/bin/Release/Nexus.app dist
  ```

* Then notarize and staple. This needs a notarytool keychain profile, created once with
  `xcrun notarytool store-credentials nexus-notary --apple-id … --team-id … --password …`:

  ```bash
  scripts/package/notarize.sh dist/Nexus-1.0.0-macOS.dmg
  ```

The CI artifact `Nexus-macOS` is ad hoc signed, **not** Developer ID signed and **not** notarized.
To run a downloaded CI build, right-click the app, choose **Open** and confirm. Alternatively,
clear the quarantine attribute on an app you trust:

```bash
xattr -dr com.apple.quarantine /Applications/Nexus.app
```

## Offline builds

Clone Luau at the pinned commit and point CMake at it:

```bash
git clone https://github.com/luau-lang/luau.git ../luau
```

```bash
git -C ../luau checkout 32c28089305f488f901aaf1dbf3fd1cb65fa3d11
```

```bash
cmake -S . -B build -G Ninja -DFETCHCONTENT_SOURCE_DIR_LUAU=$PWD/../luau
```

## Connecting Roblox Studio (optional)

1. Install or update Roblox Studio, which includes `Contents/MacOS/StudioMCP`.
2. In Studio, open **Assistant → … → Manage MCP Servers** and turn on **Enable Studio as MCP server**.
3. In Nexus, open **Roblox Integration**, click **Connect** and confirm. Nexus verifies StudioMCP's
   signature, launches it and validates its tools.
4. Choose **Roblox Studio (MCP)** in the toolbar's target picker and pick the data model (Edit,
   Server or Client during Play).
