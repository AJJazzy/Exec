# Security and privacy report

Nexus is designed security-first. This report states what it does, what it deliberately does not
do, and where its protections stop. **It does not claim that Nexus is completely safe, and it makes
no claims at all about being "undetectable"**: Nexus has nothing to hide from Roblox and does not
interact with the Roblox Player beyond read-only detection.

## Commitments (verifiable in the source)

| Requirement | How it is met |
|---|---|
| No administrator privileges | Nothing requests elevation. No helper tools, launch daemons or installers. |
| SIP and Gatekeeper untouched | No code path changes system security settings. |
| No credential or cookie access | Nexus never reads browser data, the keychain or Roblox account files. |
| No hidden telemetry | The only network request in the codebase is the **opt-in**, user-initiated version check (`AppModel.checkLatestRobloxVersion`, one GET to `clientsettingscdn.roblox.com` over an ephemeral session without cookies). It is off by default (`allowRobloxVersionCheck = false`) and is logged. |
| No unauthorized interaction with other processes | Roblox processes are only *listed* (libproc). Nexus never calls `task_for_pid`, `ptrace` or `kill` on them, and never injects into them. Process-list and bundle inspection are read-only (see `platform/macos/system_macos.cpp`). |
| No automatic execution of downloaded or imported code | Imported and opened files are copied or shown, never run. Restored drafts are opened, never run. `loadstring(game:HttpGet(...))` is flagged as an *Error*-severity remote-code finding by the analyzer. |
| No hidden background processes | The only child processes are `nexus-runner` (one per run, killed and reaped on stop) and, after the user explicitly clicks *Connect*, Roblox's own `StudioMCP`. Both belong to Nexus's process group and are stopped on exit. |
| No anti-cheat evasion or concealment | None implemented. Executor-specific APIs are recognized only so they can be reported as unavailable. |
| Proper cleanup on exit | `NexusHost::~NexusHost` cancels and joins the active session, saves history and disconnects Studio. `Subprocess` destructors kill and reap their children. |

## Execution isolation

Untrusted scripts are layered behind several independent defenses. The default target is
`luau-isolated`.

1. **Luau sandbox** (`luaL_sandbox`, `luaL_sandboxthread`): built-in libraries are read-only and each
   run gets a fresh environment. The Luau standard library has no file, process, network, `io`,
   `os.execute`, `os.exit`, `os.getenv`, `require` or `dofile` access. This is verified by
   `runtime_has_no_filesystem_or_process_access`.
2. **Runtime limits**: wall-clock timeout, memory cap enforced by the allocator (a refused allocation
   also aborts the run, so `pcall` cannot continue past it), output cap, and a cap on live
   coroutines.
3. **Process isolation**: every run uses a fresh `nexus-runner` process with a minimal environment,
   only stdin/stdout/stderr inherited (`POSIX_SPAWN_CLOEXEC_DEFAULT`) and its own process group.
4. **rlimits** in the runner: `RLIMIT_CPU` (timeout + 2 s), `RLIMIT_FSIZE = 0` (no regular-file
   writes), `RLIMIT_NOFILE = 16`, `RLIMIT_CORE = 0`, `RLIMIT_NPROC = 0`.
5. **macOS Seatbelt** (`sandbox_init(kSBXProfilePureComputation)`): no file-system writes, no
   network, no process creation. The runner reports whether it was applied, and the app shows it
   in the console. `nexus-runner --probe-sandbox` measures the effect (a file write and a socket
   connect) rather than assuming it, and the integration test asserts it on macOS.
6. **Forced termination**: SIGKILL after the grace period or the hard deadline.

### Limitations of each mechanism

* `sandbox_init`'s named profiles are deprecated in the macOS SDK. They still work on current macOS
  (verified in CI), but Apple could remove them; the runner then reports "not applied" and the
  other layers remain. The profile is applied after start-up, so it does not restrict what the
  runner did before that point (which is only reading its request).
* `RLIMIT_NPROC` is ignored for root. `RLIMIT_FSIZE` does not restrict pipes, which is intentional
  because the protocol uses them.
* Memory limits apply to the Luau heap, not to the whole process. macOS does not enforce
  `RLIMIT_AS`/`RLIMIT_DATA`.
* The in-process target has only layers 1 and 2. A script blocked in a pathological native call
  (for example an extreme string pattern) cannot be interrupted until the call returns. It is
  therefore not the default.
* On Linux (development only) there is no OS sandbox. The runner says so explicitly.
* Nexus.app itself is **not** App Sandboxed: it must enumerate processes and launch Roblox's
  StudioMCP, which the App Sandbox forbids. Its code runs with the user's privileges like any
  editor. The untrusted part (scripts) runs in the sandboxed helper.

## Roblox Studio integration

* Off until the user clicks **Connect**, which shows a dialog describing what will happen.
* Before launch, `StudioMCP`'s code signature is validated with Security.framework against the
  requirement `anchor apple generic and certificate leaf[subject.OU] = "2CFABCH843"` (Roblox's
  Apple team, as observed in the Player binary). If validation fails, the binary is not launched.
  The check can be disabled in Settings (not recommended).
* The tool schema reported by the server is inspected. If Nexus cannot map it unambiguously (for
  example after a Studio update), the integration reports UNSUPPORTED instead of guessing
  arguments.
* Scripts run on this target execute with Studio's permissions and can modify the open place. As
  Roblox's documentation warns, connect only clients you trust. Nexus cannot enforce time or
  memory limits inside Studio, and it says so in the UI.

## Data handling

* Everything stays local, under `~/Library/Application Support/Nexus`.
* Logs never contain script source. They record events, statuses and timings.
* Imports preserve bytes exactly. Text shown in the UI and written to JSON is sanitized to valid
  UTF-8.
* Library paths are confined to the library root: absolute paths, `..`, hidden names and
  symlinks pointing outside the root are rejected (`library_rejects_path_traversal_and_bad_names`).
* All writes are atomic, and deleted items are moved to `.trash` rather than erased.

## Code signing and distribution

* CI produces an **ad hoc**-signed build with the Hardened Runtime and **no** entitlement
  exceptions (`scripts/package/Nexus.entitlements` is empty). Ad hoc builds run on the building Mac;
  Gatekeeper blocks them elsewhere.
* Developer ID signing and notarization are supported by `scripts/package/package-macos.sh`
  (`NEXUS_CODESIGN_IDENTITY`) and `scripts/package/notarize.sh`. **No build has been
  Developer ID-signed or notarized as part of this work**, because no Apple Developer credentials
  were available.

## Verification performed

* Unit and integration tests: path traversal, sandbox probe (macOS), the Player's state machine
  never reaching CONNECTED, the Player target refusing execution, signature mapping, timeouts and
  kills, and bounded console and logs.
* AddressSanitizer and UndefinedBehaviorSanitizer across the full suite, with no findings.
  ThreadSanitizer on the session, runtime and integration suites, with no findings after fixing one
  test-fixture race.
* Malformed-input robustness: the Mach-O parser is exercised with every truncation and thousands of
  byte corruptions, and the JSON and plist parsers with malformed documents.
