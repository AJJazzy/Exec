# Compatibility report

## Platform support

| Item | Status |
|---|---|
| macOS | 13 Ventura or later (SwiftUI `NavigationSplitView`, `Grid` and `Table` APIs) |
| Apple Silicon (arm64) | Native. CI builds and runs the tests and the app on an arm64 macOS 15 runner. |
| Intel (x86_64) | Built as part of the universal binary (`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`, checked with `lipo -info` in CI). **Not executed on Intel hardware in this work.** |
| Linux | Core, runner, CLI and tests only (no GUI). Used for development and CI. No OS sandbox for the runner. |
| Toolchain | CMake ≥ 3.28, Xcode 15+ (Swift 5 mode), Apple clang or GCC 13 / clang 18 for the core |

## Roblox software

| Software / version | Detection | Execution |
|---|---|---|
| Roblox Player 0.745.0.7451137 (live channel, 2026-10-30) | Bundle, version, build, minimum macOS, architecture, code signature, Hardened Runtime, running PIDs, Rosetta translation, update detection | **Not supported.** No authorized interface exists (`docs/feasibility.md`). |
| Roblox Studio with the built-in MCP server (`StudioMCP`) | Bundle, version, running PIDs, StudioMCP presence and signature | Supported through `execute_luau` (Edit, Server or Client data model), after the user connects. **Tested against a mock MCP server only**; no Studio installation was available in this environment. |
| Roblox Studio without StudioMCP (older versions) | Detected | Reported UNSUPPORTED ("update Studio") |
| Legacy `studio-rust-mcp-server` (`run_code`) | — | The schema mapping accepts `run_code(command)` (unit-tested). Launching it is not wired into the UI. |

### Version-change handling

* `roblox-versions.json` records the last version seen for the Player and for Studio. When it
  changes, Nexus logs "updated from X to Y" and the Roblox view shows the previous version.
* For Studio, every connection re-reads the MCP tool list and re-derives the argument mapping. If a
  Studio update changes the tool name or adds a required parameter Nexus does not understand, the
  integration reports UNSUPPORTED with the reason instead of sending malformed requests. This is
  tested with the `changed-schema` and `no-exec` mock modes.
* The optional online check compares the installed version with Roblox's live channel. It never
  downloads or runs anything.

## Script compatibility analysis

The analyzer parses the script with the official Luau parser and walks the AST. Name resolution
comes from Luau itself, so a `local game = {}` correctly shadows the Roblox global. It reports:

| Category | Examples | Standalone | Studio | Player |
|---|---|---|---|---|
| Luau builtins | `print`, `math`, `string`, `table`, `coroutine`, `buffer`, `vector`, `typeof` | ✓ | ✓ | ✗ |
| Nexus extensions | `task` (spawn/defer/delay/wait/cancel), `warn`, `loadstring` | ✓ | ✓ (Roblox's own) | ✗ |
| Roblox globals | `game`, `workspace`, `script`, `require`, `Enum`, `shared`, `plugin` | ✗ | ✓ (`script`/`plugin` conditional) | ✗ |
| Roblox datatypes | `Instance`, `Vector3`, `CFrame`, `Color3`, `UDim2`, `TweenInfo`, … | ✗ | ✓ | ✗ |
| Deprecated globals | `wait`, `spawn`, `delay`, `tick`, `Game`, `Workspace` | ✗ | ✓ (deprecated) | ✗ |
| Services via `GetService`/`game.X` | `Players`, `RunService`, `ReplicatedStorage`, `PathfindingService`, … | ✗ | ✓ | ✗ |
| Client-only APIs | `UserInputService`, `ContextActionService`, `Players.LocalPlayer`, `RenderStepped`, `CurrentCamera` | ✗ | Partial (Client data model during Play) | ✗ |
| Restricted services | `CoreGui`, `CorePackages` | ✗ | ✗ | ✗ |
| Executor APIs | `getgenv`, `hookfunction`, `getrawmetatable`, `Drawing`, `writefile`, `request`, `game:HttpGet`, `debug.getupvalue`, … (~110 names) | ✗ | ✗ | ✗ |
| Dangerous operations | `loadstring(game:HttpGet(...))` (error), `loadstring` (warning), `require(<asset id>)`, huge `string.rep`, `getfenv`/`setfenv` | — | — | — |
| Runtime assumptions | `while true do end` without a yield or exit | warning | warning | — |
| Unknown globals | anything not defined by the script or the catalogue | unknown | unknown | — |

Each finding carries a severity, source line, occurrence count, per-target availability and, where
relevant, a risk statement. Verdicts are computed per target, and **the Roblox Player verdict is
always "unavailable"**.

### What the standalone runtime implements and what it does not

* Implemented with real behaviour (not stubs): `print`, `warn`, `task.*`, `loadstring`, and the
  full Luau standard library from Luau 0.744. The list is enumerated from a live VM, so it always
  matches the linked version.
* `task.wait()` has a minimum resolution of one 60 Hz frame (1/60 s), as in Roblox, and returns the
  elapsed time.
* `_G` is a writable table shared by a run's threads and separate from the script's globals, as in
  Roblox.
* **Not emulated**: `game`, `Instance`, `Vector3`, `CFrame` and every other Roblox engine API. They
  are `nil` in the standalone runtime (tested), and the analyzer explains why before you run.
  Faking them would let scripts "succeed" without doing anything.

## Known limitations

* The analyzer is static: code produced at runtime (`loadstring`, `require` of assets) is reported as
  unanalysed, not analysed.
* Luau type annotations are parsed, but the type checker (`Luau.Analysis`) is not linked.
* Highlighting runs on the main thread, debounced after each edit. On the Linux test machine a
  debug build tokenises ~8,750 lines in ~73 ms (see `docs/testing.md`), so very large files
  (≫ 20k lines) will feel sluggish while typing.
* The minimap shows line shapes, not syntax colours.
