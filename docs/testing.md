# Test results

All numbers below were produced by running the suites; nothing is estimated. Tests that use
simulated components carry **MOCK** or **SIMULATED** in their names and print a notice when they
run. Passing them shows that Nexus's logic is correct. It does **not** show compatibility with
real Roblox software.

## Suites

| Executable | Label | Cases | What it exercises |
|---|---|---|---|
| `test_core` | unit | 10 | JSON (round-trip, escapes, surrogates, depth limit, malformed input), UTF-8 validation and sanitising, ring buffer bounds, cancellation wake-up, bounded logger and export, compiler success, multi-error syntax diagnostics, compile errors, UTF-16 source maps |
| `test_runtime` | unit | 20 | Real Luau execution: output capture, runtime errors with line numbers and tracebacks, compile errors not executed, infinite-loop timeout, `pcall` cannot swallow timeout or memory abort, memory and output limits, cross-thread cancellation (busy and sleeping), `task` ordering, cancel and background errors, `loadstring`, sandbox (read-only builtins, `_G` semantics, no leakage between runs), no FS/process APIs, Roblox globals absent rather than faked, 200 repeated runs with stable memory, result JSON round-trip |
| `test_analysis` | unit | 15 | Analyzer (pure Luau, Roblox globals and services, client-only APIs, executor APIs, remote `loadstring`, `require(assetId)`, scoping and user globals, unbounded loops, syntax errors), highlighter classes and bounds, formatter (blocks, closures/tables, long strings verbatim, idempotence, **identical bytecode** before and after, CRLF, rejection of broken code), completion, bracket matching |
| `test_storage` | unit | 8 | Library create/read/write/rename/duplicate/move/trash, metadata persistence across renames, path-traversal/symlink/hidden-name rejection, search, **byte-exact import/export** (CRLF and invalid UTF-8), import never executes, size and type limits, drafts with corrupt-file skipping, settings validation and clamping |
| `test_sessions` | unit | 8 | State sequence IDLE→INITIALIZING→COMPILING→READY→RUNNING→STOPPED, concurrent-run rejection, stop latency, compile errors fail without running, unknown target, restart and history, bounded console, **MOCK** target disconnect → DISCONNECTED, shutdown |
| `test_roblox` | unit | 11 | XML plist parser, Mach-O parser (thin, universal, signature, entitlements, Team ID), **fuzz-style truncation and corruption** robustness, optional real-binary check, connection state machine (the Player can never reach CONNECTED), version comparison and update history, **MOCK** Player monitor (install, run, restart generations, target refuses), bundle validation, subprocess round-trip and kill without zombies, process listing |
| `test_integration` | integration | 15 | Real `nexus-runner`: execution, errors, timeout, prompt stop, **SIGKILL of a script stuck in a native call**, 25 repeated runs leaving no processes, sandbox probe (measured), missing runner. Real `nexus-cli`: run/check/analyze/format/inspect exit codes, Player target exit 3. **SIMULATED CLIENT**: a process named `RobloxPlayer` inside a fixture bundle is detected with the right PID and version, state UNSUPPORTED, and is left untouched. Host end-to-end with library, diagnostics export and history persistence. **MOCK** Studio MCP server: connect, schema mapping, execution, error mapping, schema changes → UNSUPPORTED, no studios → INTEGRATION_AVAILABLE, timeout, cancel, server crash → generation change and ERROR. Tool mapping rules |
| `test_performance` | performance | 6 | Compile throughput, VM speed, in-process and isolated start-up latency, editor services on a large file, 200k-line output flood through the session pipeline |

**Total: 93 test cases in 8 executables.**

## Results — Linux development machine (Ubuntu 24.04, x86_64, clang 18, Debug)

```
test_core         10 passed, 0 failed, 0 skipped
test_runtime      20 passed, 0 failed, 0 skipped
test_analysis     15 passed, 0 failed, 0 skipped
test_storage       8 passed, 0 failed, 0 skipped
test_sessions      8 passed, 0 failed, 0 skipped
test_roblox       10 passed, 0 failed, 1 skipped   (real-binary check needs NEXUS_TEST_ROBLOX_BINARY)
test_integration  15 passed, 0 failed, 0 skipped
test_performance   6 passed, 0 failed, 0 skipped
ctest: 100% tests passed, 0 tests failed out of 8
```

The skipped check was then run against the real Roblox Player 0.745.0.7451137 executables downloaded
from Roblox's CDN (`NEXUS_TEST_ROBLOX_BINARY=…/RobloxPlayer`): **passed for both arm64 and x86_64**
(Team ID 2CFABCH843, Hardened Runtime set).

On Linux the sandbox probe honestly reports `sandboxApplied: false` (there is no OS sandbox
there), and the test asserts exactly that.

### Sanitizers (GCC 13)

| Build | Result |
|---|---|
| AddressSanitizer + UndefinedBehaviorSanitizer (`detect_leaks=1`, UB non-recoverable), all 8 suites | **100% passed; no memory errors, leaks or UB reported** |
| ThreadSanitizer: `test_sessions`, `test_runtime`, `test_integration` | First run: 7 race reports, all in the `test_sessions` *fixture*, which destroyed its listener's vector before joining the session worker. Fixed by member order and an explicit shutdown. Re-run 3 times: **0 reports** |

### Performance (Linux, Debug build, single run)

| Measurement | Result |
|---|---|
| Compile a ~97 KB / ~4,400-line script | 43 ms |
| `fib(27)` in the in-process VM (≈1.27 M interrupt checks) | 69 ms |
| In-process run: VM create + compile + run + close | 0.15 ms |
| Isolated run: spawn + rlimits + run + exit | 3.8 ms |
| ~8,750 lines: highlight / format / analyze / syntax check | 73 / 43 / 25 / 24 ms |
| 200,000 printed lines through runtime → session → console | 432 ms; console capped at 5,000 lines |

## Results — macOS (GitHub Actions `macos-15`, Apple Silicon, Xcode 16.4)

See the CI section below. Populated from the CI run that built the universal app.

## Reproducing

```bash
scripts/build/build.sh Debug
```

```bash
CXX=g++ CC=gcc scripts/build/sanitizers.sh
```

```bash
NEXUS_TEST_ROBLOX_BINARY=/Applications/Roblox.app/Contents/MacOS/RobloxPlayer build/debug/bin/test_roblox macho_inspects_real
```

```bash
build/xcode/bin/Release/Nexus.app/Contents/MacOS/Nexus --self-test /tmp/nexus-selftest/result.json --data-dir /tmp/nexus-selftest-data
```

The last command launches the real app, exercises the editor, formatter, completion, analyzer, an
isolated run, error reporting, stop, the in-process target, the Player-target refusal, Roblox
detection and the library, then writes the results and a snapshot of each section, and quits.
