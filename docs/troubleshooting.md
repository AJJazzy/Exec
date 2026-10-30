# Troubleshooting

Diagnostics → **Export Diagnostics…** writes one JSON file containing the environment, targets,
settings, the Roblox and Studio status, execution history and logs. Attach it to bug reports. It
contains no script source.

| Symptom | Cause and fix |
|---|---|
| **"Live execution in the Roblox Player is not supported"** | Expected. No authorized interface exists (see `docs/feasibility.md`). Use the Roblox Studio target or the Luau runtime. |
| Roblox Player shows **UNSUPPORTED** while Roblox is running | Correct: the process was detected, but detection is not attachment. |
| Player shows **DISCONNECTED** although Roblox is installed | Nexus looks in `/Applications`, `~/Applications` and Launch Services. For custom locations, set `NEXUS_ROBLOX_SEARCH_PATHS=/path/to/dir` (colon-separated) when launching from Terminal. |
| Studio shows **UNSUPPORTED: does not include StudioMCP** | Update Roblox Studio. The built-in MCP server ships with recent versions. |
| Studio shows **INTEGRATION_AVAILABLE** after Connect, "reports no open Studio instance" | Open a place in Studio, enable *Studio as MCP server* (Assistant → … → Manage MCP Servers), then click Connect again. |
| Studio **ERROR: Refusing to launch StudioMCP: its code signature could not be verified** | The binary is not signed by Roblox's team (2CFABCH843), or it is damaged. Reinstall Studio. Only if you understand the risk, turn off *Only launch StudioMCP when signed by Roblox* in Settings. |
| Studio **UNSUPPORTED: StudioMCP is not compatible with this version of Nexus** | A Studio update changed the MCP tool schema. The message lists the parameters Nexus could not map. Nexus refuses rather than guessing; please report it. |
| Isolated target **unavailable: nexus-runner executable not found** | The app bundle is incomplete. Rebuild, or check `Nexus.app/Contents/MacOS/nexus-runner`. For the CLI, keep `nexus-runner` next to `nexus-cli` (or pass `--runner PATH`). |
| Console says **sandbox: not applied** | The OS sandbox could not be applied (always the case on Linux; on macOS the detail explains why). Resource limits and process isolation still apply. |
| Script stops with **exceeded the … time limit** | Raise the limit in Settings → Execution, or add `task.wait()` to long loops. |
| **exceeded the … memory limit** | The Luau heap reached the configured limit (Settings → Execution). |
| **Output limit … reached** | Output beyond the cap is discarded. Raise *Output limit* or print less. |
| Stop is slow on the **in-process** target | A native call (for example an extreme string pattern) cannot be interrupted. Use the isolated target, which is killed after the grace period. |
| `attempt to index nil with 'GetService'` in the standalone runtime | `game` does not exist outside Roblox. Run *Check Compatibility* to see Roblox dependencies, and use the Studio target. |
| App does not open: **"cannot be opened because the developer cannot be verified"** | The CI build is ad hoc signed and not notarized. Right-click → Open, or build locally (`docs/installation.md`). |
| Drafts reappear after restart | Unsaved buffers are restored (never executed). Save or close them, or turn off *Restore unsaved drafts*. |
| Logs | `~/Library/Application Support/Nexus/Logs/nexus.log` (JSON lines; rotated to `nexus.log.1` at 4 MB). |
| Reset everything | Quit Nexus and delete `~/Library/Application Support/Nexus`. |

## Command-line diagnostics

```bash
nexus-cli diagnostics
```

```bash
nexus-cli roblox
```

```bash
nexus-cli targets
```

```bash
nexus-runner --probe-sandbox
```

The last command measures whether file writes and network connections are blocked inside the
runner sandbox.
