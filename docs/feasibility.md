# Live Roblox Player execution on macOS — feasibility report

**Conclusion: executing scripts inside the live Roblox Player on macOS is not available through any
supported or authorized interface. Nexus therefore does not implement it, never reports it as
available, and offers the two authorized alternatives instead (Roblox Studio through Studio's
official MCP server, and the standalone Luau runtime).**

Research date: 2026-10-30. Roblox Player version examined: **0.745.0.7451137**
(`clientVersionUpload` `version-b09c93298b5d4cd1`), the live macOS channel on that date.

## Method

1. Queried Roblox's public client-version endpoint
   (`https://clientsettingscdn.roblox.com/v2/client-version/MacPlayer`) for the current live
   version.
2. Downloaded the official macOS Player packages from Roblox's CDN
   (`setup.rbxcdn.com/mac/…-RobloxPlayer.zip` for x86_64 and `setup.rbxcdn.com/mac/arm64/…` for
   arm64) and inspected them **on disk only**: `Info.plist`, Mach-O headers and the embedded code
   signature (CodeDirectory flags, Team ID, entitlements). The same inspection is built into Nexus
   (`nexus-cli inspect`, Roblox Integration view), so anyone can reproduce it on their own install.
3. Reviewed Roblox's creator documentation for scripting and integration surfaces
   (`loadstring`, Studio MCP server, Open Cloud Luau Execution).
4. No Roblox process was attached to, modified or tampered with at any point.

## 1. Confirmed technical facts (measured or documented)

| Fact | Evidence |
|---|---|
| Live macOS Player version is 0.745.0.7451137 | Roblox client-version endpoint; `CFBundleShortVersionString` in the downloaded bundle |
| Bundle id `com.roblox.RobloxPlayer`, executable `RobloxPlayer`, installed as `/Applications/Roblox.app` | `Info.plist` |
| Roblox ships **separate single-architecture packages**: x86_64 (`mac/`) and arm64 (`mac/arm64/`) | Mach-O headers of both downloads (no universal binary) |
| arm64 build: minimum macOS 11.0, SDK 26.2; x86_64 build: minimum macOS 10.13, SDK 26.2; `LSMinimumSystemVersion` 10.13 | `LC_BUILD_VERSION` / `LC_VERSION_MIN_MACOSX`, `Info.plist` |
| Signed by Apple Developer **Team ID 2CFABCH843**, identifier `com.roblox.RobloxPlayer` | CodeDirectory |
| **Hardened Runtime is enabled** (CodeDirectory flag `0x10000`) on both architectures | CodeDirectory flags |
| Entitlements are only `disable-executable-page-protection`, `device.audio-input`, `device.camera` | embedded entitlements blob |
| There is **no** `com.apple.security.get-task-allow` entitlement | entitlements |
| There is **no** `com.apple.security.cs.allow-dyld-environment-variables` entitlement | entitlements |
| There is **no** `com.apple.security.cs.disable-library-validation` entitlement | entitlements |
| In Roblox, `loadstring()` is disabled by default and is controlled by `ServerScriptService.LoadStringEnabled` (a server-side setting) | create.roblox.com `LuaGlobals.loadstring` |
| Roblox Studio includes an official MCP server (`/Applications/RobloxStudio.app/Contents/MacOS/StudioMCP`, stdio transport) with an `execute_luau` tool taking a data model type of Edit, Client or Server | create.roblox.com/docs/studio/mcp |
| Roblox's earlier standalone Studio MCP server (`Roblox/studio-rust-mcp-server`, `run_code` tool) is archived; Roblox recommends the built-in server | GitHub repository notice |
| Open Cloud offers a Luau Execution API that runs Luau in a server instance of a place you own, via an API key | create.roblox.com Open Cloud reference |

What the signature facts mean on macOS (Apple platform behaviour, applied to the measured flags):

* With the Hardened Runtime and no `get-task-allow`, macOS refuses to give other processes the
  Player's task port, so debuggers and memory-writing tools cannot attach (short of disabling System
  Integrity Protection).
* Without `allow-dyld-environment-variables`, `DYLD_INSERT_LIBRARIES` is ignored for the Player.
* With library validation in force, the Player only loads libraries signed by Apple or by Roblox's
  team.

**Every technique for running foreign code inside the Player would therefore require defeating
operating-system protections (for example disabling SIP, re-signing or patching the binary) or
exploiting the client.** Those are exactly the actions this project is required not to perform, and
Roblox's Terms of Use prohibit modifying or interfering with the client.

## 2. Reasonable engineering hypotheses (not verified)

* Roblox's Hyperion anti-tamper technology may be present in the macOS Player. The binary contains
  no plaintext "Hyperion"/"Byfron" markers, which is **inconclusive** (anti-tamper code does not
  label itself). Public reporting was contradictory and low-quality, so Nexus makes no claim either way.
  The conclusion above does not depend on it: the macOS code-signing protections alone already
  rule out authorized injection.
* The `disable-executable-page-protection` entitlement suggests the Player generates or patches
  executable code at runtime (e.g. a JIT for native Luau code generation). Nexus does not rely on this.
* Roblox Studio's `StudioMCP` is probably signed by the same team (2CFABCH843). Studio's macOS
  payload is downloaded by its installer, so the binary could not be inspected for this report.
  Nexus therefore **verifies the signature at connection time** and refuses to launch StudioMCP if it
  is not Developer ID-signed by that team (configurable, on by default).

## 3. Unsupported assumptions explicitly rejected

* *"Windows executor techniques carry over to macOS."* They do not: Windows executors rely on
  process-memory access and DLL injection, whereas the macOS Player's Hardened Runtime, missing
  `get-task-allow` and library validation block the macOS equivalents.
* *"A running Player process means Nexus is connected."* Detection is not attachment. Nexus's
  Player state machine cannot even represent INTEGRATION_AVAILABLE or CONNECTED: those transitions
  are rejected in code and covered by tests.
* *"Studio execution equals live-game execution."* It does not. Studio runs your place locally
  (Edit, or a Play test's Server/Client data models). It is not a live server, and other players
  are not involved.

## 4. Unavailable functionality

| Capability | Status in Nexus |
|---|---|
| Execute scripts in the live Roblox Player | **Unavailable** (no authorized interface) |
| Read or modify Player memory, hook functions, executor APIs (`getgenv`, `hookfunction`, `Drawing`, …) | **Unavailable**, and flagged by the compatibility analyzer |
| Attach a debugger to the Player | **Unavailable** (blocked by macOS, and not attempted) |
| Detect Player installation, version, architecture, signature, running PIDs and Rosetta status | **Implemented** (read-only) |
| Execute Roblox engine code in Roblox Studio (Edit/Server/Client) | **Implemented** through the official MCP server. Validated against a mock server only, because no Studio installation was available in the build environment |
| Execute plain Luau with time, memory and output limits | **Implemented** (official Luau VM 0.744) and tested |
| Open Cloud Luau Execution | **Not implemented** (researched only; it needs a user API key and a published place, see roadmap) |

## Reproducing the measurements

```bash
nexus-cli inspect /Applications/Roblox.app
nexus-cli roblox
```

The Roblox Integration view in the app shows the same data. Its "Check Latest Version" button
makes the single, opt-in request to the public version endpoint.
