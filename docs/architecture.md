# Architecture

Nexus is a native macOS application built around a portable C++20 core. The UI language does not
shape the core: SwiftUI/AppKit only talk to an Objective-C bridge, and the same core powers
`nexus-cli` and the tests on macOS and Linux.

```
┌──────────────────────────── Nexus.app ────────────────────────────┐
│ SwiftUI shell (ui/app)          AppKit editor (TextKit 1)          │
│   sidebar · toolbar · views     highlighting · gutter · minimap    │
│              │                                                     │
│   Objective-C++ bridge (ui/bridge/NXBridge.mm)  ── main-queue events│
└──────────────┼─────────────────────────────────────────────────────┘
               ▼
        host/ NexusHost  (composition root, shared with nexus-cli)
   ┌───────────┼───────────────────────────────┬──────────────────────┐
   ▼           ▼                               ▼                      ▼
sessions/   execution/ targets            integrations/          library/, settings/
SessionMgr  ├ luau-isolated ──spawn──►     ├ roblox/ (read-only    diagnostics/ Logger
            │   nexus-runner process       │   detection, Mach-O,
            ├ luau-inprocess               │   state machines)
            ├ roblox-studio ──stdio──►     └ studio/ MCP client ──► StudioMCP (Roblox)
            └ roblox-player (always unavailable)
               │
   compiler/ · runtime/ (official Luau 0.744: Ast, Compiler, VM) · compatibility/ · editor/
               │
          platform/  posix_spawn, libproc, Security.framework, Seatbelt, Launch Services
```

## Modules

| Directory | Library | Responsibility |
|---|---|---|
| `platform/` | `nexus_platform` | RAII subprocesses (`posix_spawn`, pipes, process groups), read-only process enumeration (libproc / `/proc`), Rosetta detection, code-signature validation (Security.framework), self-sandboxing (`sandbox_init`), rlimits, Launch Services lookup, CF property lists. Apple-only features report "unsupported" on Linux instead of pretending to work. |
| `core/common` | `nexus_core` | `Result<T>`, JSON DOM, UTF-8 helpers, cancellation tokens (callback- and condition-variable based, no polling), bounded ring buffer. |
| `core/diagnostics` | | Structured diagnostics, thread-safe bounded logger with JSON-lines file sink, rotation and export. |
| `core/compiler` | | Luau parse/compile front end. All parse errors are reported with source ranges. Byte↔UTF-16 source maps. |
| `core/runtime` | | Sandboxed Luau VM (`luaL_sandbox` + per-run thread environment), output capture, `task` scheduler, `loadstring`, interrupt-based timeout and cancellation, allocator-enforced memory cap, output cap, statistics. |
| `core/execution` | | `ExecutionTarget` interface, in-process and isolated targets, runner wire protocol. |
| `core/sessions` | | Session state machine, single active session, console buffer, history, stale-session handling. |
| `core/compatibility` | | Symbol and service catalogue, AST-based analyzer, per-target verdicts. |
| `core/editor` | | Lexer-based highlighting, a formatter that preserves bytecode, completion, bracket matching. |
| `core/library`, `core/settings` | | Script library, drafts, validated settings. |
| `integrations/roblox` | `nexus_integrations` | XML plist reader, Mach-O/code-signature parser, client discovery, connection state machine, version history, Player monitor and the (always unavailable) Player target. |
| `integrations/studio` | | MCP client (JSON-RPC over stdio), Studio integration with schema-driven tool mapping. |
| `host/` | `nexus_host` | Wires everything together; persistence of settings, history, versions and logs. |
| `runner/` | `nexus-runner` | Isolated executor: one request per process, rlimits, Seatbelt sandbox, JSON-lines protocol. |
| `cli/` | `nexus-cli` | Command-line front end (run, check, analyze, format, roblox, studio, inspect, targets, diagnostics). |
| `ui/` | `Nexus.app` | SwiftUI shell, AppKit editor and console, Objective-C++ bridge, `--self-test` mode. |

Dependencies: **Luau 0.744** (MIT, pinned by commit in `cmake/NexusLuau.cmake`) is the only
third-party library. JSON, the test harness and the plist/Mach-O readers are in-tree to keep the
dependency and attack surface small.

## Execution flow

1. **UI or CLI** calls `NexusHost::run(source, name, target)`, which builds an `ExecutionRequest`
   with the configured limits.
2. **SessionManager** refuses the run if another session is active (`session.busy`). Otherwise it
   moves to INITIALIZING, checks target availability, moves to COMPILING (a local syntax check for
   every target, so syntax errors are never sent anywhere), then READY → RUNNING on a worker thread.
3. The **target** executes:
   * `luau-isolated` spawns `nexus-runner --serve-one` and writes the request. The runner applies
     rlimits and the Seatbelt sandbox, reports `ready`, streams `output` messages and ends with a
     `result` message. The host polls the pipes together with a self-pipe that cancellation wakes,
     so nothing busy-waits.
   * `luau-inprocess` runs the same runtime on the worker thread.
   * `roblox-studio` calls the mapped MCP tool (`execute_luau`) and converts its content to output.
   * `roblox-player` always returns `Unavailable`.
4. Output events go to the bounded console buffer and to listeners. The bridge coalesces them to at
   most ~30 UI updates per second, regardless of output volume.
5. The session ends as STOPPED (completed or stopped by the user), FAILED, or DISCONNECTED (the
   target's connection generation changed mid-run). A record is appended to the history, which is
   persisted.

### Stopping scripts

* Runtime: the Luau interrupt callback runs at every safepoint (loop back-edges, calls, returns)
  and checks an atomic cancel flag; the clock is checked every 1024 safepoints. Once tripped, it
  raises an error at **every** later safepoint, so `pcall` cannot swallow it. Sleeping in
  `task.wait` waits on the cancellation condition variable and wakes immediately.
* Isolated runner: cancellation sends `{"type":"cancel"}`. If the runner has not exited within the
  grace period (default 750 ms), it is SIGKILLed. A hard deadline (timeout + grace + 2 s) also
  catches scripts stuck inside a single native call, and RLIMIT_CPU is a final kernel backstop.
* In-process: cooperative only. A script blocked in one native call stops only when that call
  returns, which is why the isolated target is the default.
* Studio: a `notifications/cancelled` message is sent. Nexus reports that Studio may keep running
  the script, because MCP does not guarantee termination.

### Connection state machines

States: DISCONNECTED, DISCOVERING, CLIENT_FOUND, CHECKING_COMPATIBILITY, INTEGRATION_AVAILABLE,
CONNECTED, UNSUPPORTED, ERROR. Transitions are validated (`ConnectionStateMachine::isLegal`).
The Player's machine is built with `integrationSupported = false`, so INTEGRATION_AVAILABLE and
CONNECTED are unreachable for it. Each target exposes a *generation* counter that changes when the
underlying connection is replaced (Studio reconnect or crash, Player restart), and the session
manager uses it to mark stale runs DISCONNECTED.

Process launches and exits are observed in the app through key-value observation of
`NSWorkspace.runningApplications`, debounced to a single refresh. There is no polling loop.

## Threading model

| Thread | Work |
|---|---|
| Main (AppKit) | UI, highlighting (fast, debounced), language services |
| Session worker (one at a time) | target execution; joined before the next session starts and on shutdown |
| Runner/MCP reader | the MCP client's stdout/stderr reader thread; the isolated target polls on the worker |
| GCD utility queue | Roblox/Studio refreshes, compatibility analysis |

Shared state is protected by mutexes. Listener callbacks are invoked outside locks. Every
long-lived thread is joined on shutdown. The test suites pass under ThreadSanitizer and
AddressSanitizer/UBSan (see `docs/testing.md`).

## Persistence

`~/Library/Application Support/Nexus/` holds `settings.json`, `history.json`,
`roblox-versions.json`, `Library/` (scripts, `.nexus-library.json` metadata, `.trash/`), `Drafts/`
and `Logs/nexus.log` (JSON lines, rotated at 4 MB). Every write is atomic (temporary file and rename).
