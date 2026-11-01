# LakeShark MCP

Let an MCP client read the SDK, create and edit apps, build, simulate, see PNG
screenshots, and install through LakeShark Desktop or USB serial.

```powershell
python app-sdk/lsdk.py mcp
```

This is a **stdio subprocess**, not an HTTP server. It prints only newline-delimited
UTF-8 JSON-RPC to stdout; diagnostics go to stderr. Exit by closing stdin. No API
key, account, network listener, or secret is needed. The official `mcp` Python
package was unavailable in the anaconda environment, so this implementation uses
the minimal stdio protocol directly. Supported protocol versions: `2024-11-05`,
`2025-03-26`, `2025-06-18`, `2025-11-25`; unsupported client versions negotiate
`2025-11-25`. Tools return JSON in text content for compatibility with older clients,
plus native MCP image content for screenshots.

## Setup

Python 3.9+ runs the server. For screenshots use a Python environment with Pillow;
the provided anaconda Python already has it. A board build needs Espressif's RISC-V
compiler (see the [SDK README](../README.md)); real simulation needs the bench,
CMake, Ninja and a host C compiler. Direct board access needs pyserial. There is
no package installation step for MCP itself.

By default apps live in `app-sdk/apps`, independent of the client's current
directory. Use `--workspace ABSOLUTE_DIRECTORY` or `LSDK_WORKSPACE` to select
another app workspace. `app` arguments are **folder names**, not arbitrary paths.
`list_apps` lists these local folders; use `board_console("app list")` to list
installed board apps.

All examples below are ready to copy on this worktree. On another machine,
replace the Python executable and repository paths with absolute paths. A client
starts its own server process; do not start a separate server in a terminal first.

## Claude Code

From PowerShell, register the subprocess for this project:

```powershell
claude mcp add --transport stdio --scope project lakeshark -- python <LakeShark folder>/app-sdk/lsdk.py mcp
claude mcp get lakeshark
```

Alternatively merge this into the repository root `.mcp.json`:

```json
{
  "mcpServers": {
    "lakeshark": {
      "type": "stdio",
      "command": "python",
      "args": ["<LakeShark folder>/app-sdk/lsdk.py", "mcp"]
    }
  }
}
```

Open `/mcp` in Claude Code to check the connection. See the
[Claude Code MCP documentation](https://code.claude.com/docs/en/mcp).

## Claude Desktop

Open Settings → Developer → Edit Config. Merge the following into
`%APPDATA%/Claude/claude_desktop_config.json` on Windows, or
`~/Library/Application Support/Claude/claude_desktop_config.json` on macOS,
then fully restart Claude Desktop:

```json
{
  "mcpServers": {
    "lakeshark": {
      "command": "python",
      "args": ["<LakeShark folder>/app-sdk/lsdk.py", "mcp"]
    }
  }
}
```

Look for LakeShark in the tool selector. See the
[local server setup guide](https://modelcontextprotocol.io/docs/2026-07-28/develop/connect-local-servers).

## Codex CLI

Merge this into `~/.codex/config.toml` (Windows: `%USERPROFILE%/.codex/config.toml`):

```toml
[mcp_servers.lakeshark]
command = "python"
args = ["<LakeShark folder>/app-sdk/lsdk.py", "mcp"]
startup_timeout_sec = 20
tool_timeout_sec = 300
```

Run `codex mcp list`, then start Codex. The larger tool timeout permits initial
compiler/simulator builds. See the
[official OpenAI MCP documentation](https://developers.openai.com/codex/mcp/).

## VS Code

Merge into `.vscode/mcp.json`:

```json
{
  "servers": {
    "lakeshark": {
      "type": "stdio",
      "command": "python",
      "args": ["${workspaceFolder}/app-sdk/lsdk.py", "mcp"]
    }
  }
}
```

Use **MCP: List Servers**, start LakeShark, and enable its tools in the chat tool
picker. `${workspaceFolder}` must be the LakeShark repository root. See the
[VS Code MCP configuration reference](https://code.visualstudio.com/docs/agents/reference/mcp-configuration).

## Tools and schemas

[tools.json](tools.json) contains the full JSON input schemas and descriptions,
exactly as returned by `tools/list`. Every input object rejects extra properties.
The signatures below summarize those schemas; `?` means optional.

| Tool | Inputs | Result |
| --- | --- | --- |
| `sdk_docs` | `query: string` | Matching README sections and API signatures/doc comments; counts before truncation |
| `sdk_api` | `symbol: string` | Exact case-sensitive symbol matches: `name, kind, signature, doc, header` |
| `new_app` | `name: string, template?: blank\|radio\|dots\|3d\|service` (default `blank`) | New app folder and path |
| `list_apps` | `{}` | Workspace apps, declared IDs and relative files |
| `read_file` | `app: string, path: string` | UTF-8 `content` |
| `write_file` | `app: string, path: string, content: string` | Relative path and byte count |
| `build` | `app: string` | `ok, problems, output, lap, manifest, size` |
| `sim_run` | `app: string, frames?: integer 1..2000` (60), `script?: string, orientation?: portrait\|landscape` (portrait) | Final PNG image, simulator/build log, replay script and frame count |
| `sim_input` | `app: string, events: Event[1..100]` | Replay through the new input, final PNG and log |
| `install` | `app: string, open?: boolean` (true) | Transport, upload progress, reply, and open result |
| `board_console` | `line: string, confirm?: boolean` (false) | Reply lines/text and the guarded command list |
| `board_screen` | `{}` | PNG image after byte count and CRC-32 validation |
| `board_input` | `events: Event[1..100]` | Current board PNG after injecting input |
| `board_logs` | `app?: string, n?: integer 1..1000` (100) | Last available lines, optional exact `[app ID]` filter |

Build diagnostics are objects with `file: string, line: integer, col: integer,
severity: error|warning|note, message: string`. `lap` is an absolute `.lap` path
on success; `size` is its byte count. A failed build or tool operation sets MCP
`isError: true`. Operational errors stay inside a tool result; protocol errors use
JSON-RPC error responses. A failed upload retains its progress. If upload succeeds
but opening fails, `installed: true` and `open_error` make that distinction explicit.
Install also emits `notifications/progress` if the caller supplies
`params._meta.progressToken`.

`Event` has a required `type` and the following fields:

| `type` | Required fields | Supported by |
| --- | --- | --- |
| `key` | `key`: a printable single-byte character, or `@up`, `@down`, `@left`, `@right`, `@enter`, `@tab` | Sim and board; board also supports `@esc`, `@backspace` |
| `tap` | `x, y`: zero-based pixels | Sim and board; generates press/release |
| `down`, `up` | `x, y` on board; sim `up` needs no fields | Sim and board |
| `tilt` | `ax, ay`, optional `az` (1), finite numbers -100..100; nonzero vector | Sim |
| `swipe` | `direction: U\|D\|L\|R`; board also requires starting `x, y` | Sim and board |
| `rx` | `count`: integer 0..100000 | Sim |
| `release_all` | No fields | Board |

Optional `frame: integer 1..2000` schedules a sim event relative to the previous
run's last frame. Otherwise events are four frames apart. A sim tap releases two
frames later. Board events reject `frame`; board swipes move 64 pixels. Coordinates
are integers 0..32767. Optional board `col, row` override cell coordinates (default:
`x / 10`, `y / 17`, rounded down, the board's default font); supply these for a
different font/grid. Board input uses the actual 24-byte little-endian wire layout
in `components/apps/tui/ls_input_ev.h`. Lines carry at most three records to fit
the existing bridge's 200-character limit.

Example tool arguments:

```json
{"app":"demo","frames":60,"script":"5:key=o;20:tilt=0.3,-0.1;40:swipe=L","orientation":"landscape"}
```

```json
{"app":"demo","events":[{"type":"key","key":"o"},{"type":"tap","x":120,"y":300},{"type":"rx","count":4}]}
```

## Board access and console guard

No board is contacted on server startup, documentation lookup, file operations,
build or simulation. Board tools use `lapboard.link()`: the desktop bridge on
`127.0.0.1` when running, otherwise USB serial. `LSDK_BRIDGE_PORT` overrides the
bridge port (default 8770 or the desktop configuration). If the bridge is running
but reports no board, the tool returns that error instead of competing for serial.
Start the desktop bridge from this checkout so it shares the quiet console mode
added to `lapboard.do_rpc`.

`board_console` accepts one line, at most 200 UTF-8 bytes. Control characters and
`;`, `|`, `&` are refused even with confirmation. These command words/categories
require literal JSON `"confirm": true`:

- Formatting: `format`, `formatting`, `mkfs`, `erase`.
- Deleting files/apps: `sd rm|remove|delete`, `app rm|remove|delete`.
- Clearing crash records: `crumb clear`, `crash clear`.
- Flashing/updating: `flash`, `flashing`, `ota`, `update`, `bootloader`.
- Transmitting/arming: `tx`, `transmit`, `send`, `ptt`, `arm`, `say`, `mesh dm`, `mesh advert`.
- Enabling/testing addressed radio reports: `cellreport target`, `cellreport test`.
- Restart/reset: `reset`, `reboot`, `factory`.

Matching is case-insensitive and deliberately conservative, including command
words in arguments. The refusal result lists the guarded categories. This guard
covers raw console calls; installing apps and injecting UI events remain explicit
board actions. It is not a sandbox for native app code.

## Resources

`resources/list` exposes `lakeshark://sdk/README.md` plus
`lakeshark://sdk/include/<header>.h` for every SDK include header, read-only.
`resources/read` accepts only those exact URIs. `sdk_docs` searches the README
and the existing `apidoc` index of `lakeshark_app.h`, `lakeshark_ui.h`,
`lap_canvas.h`, and `lap3d.h`. Signatures preserve the header's types/arguments
with normalized whitespace; inline bodies are omitted.

## Host tests

```powershell
python -m unittest discover -s app-sdk/mcp/tests -v
```

The suite launches `lsdk mcp` over actual stdin/stdout pipes. Each tool is called.
The real `lsweb` HTTP handler is bound to **127.0.0.1 on an ephemeral port** and
connected to a fake serial device. A separate test uses `SerialLink` with a fake
opener and no bridge. Port discovery raises if invoked. The fixture compiler emits
a tiny ELF consumed by the real manifest parser; compilation failures exercise
the real diagnostic parser. A stub lssim runs as a subprocess with the real batch
arguments/environment and produces a BMP, converted to PNG by the server.

Assertions cover schemas, resources, UTF-8, every tool, image decoding, corrupt PNG
CRC, install SHA-256 failure/progress, command guards, malformed JSON/protocol
requests, app traversal/drive/UNC/ADS/device paths, hard links, symlink escapes
(skipped if the OS forbids creating symlinks), replay timing and limits. Tests
reuse the existing SDK install test's fake upload protocol. They do not open a
board or COM port, scan ports, or need the RISC-V toolchain.

## Limits

- This fallback implements initialization, ping, tools, resources, progress and
  errors. Requests run sequentially. It does not implement concurrent cancellation,
  MCP tasks, prompts, resource subscriptions, HTTP transport, or server-initiated
  sampling. Newer clients must accept a supported negotiated protocol version.
- Simulation uses batch replay, not a persistent `--feed` session. Each input call
  rebuilds and starts from frame one with the accumulated script, up to 2000 frames.
  Replay depends on app and simulator determinism; wall-clock/external state may
  vary. Only the final frame is returned to keep responses small. Batch execution
  times out after 120 seconds; first compiler/CMake builds use existing SDK behavior
  and can take longer. Configure an adequate client tool timeout.
- `sim_input` supports the documented key/touch/tilt/swipe/rx events. Board tilt/rx
  synthesis is unavailable. UI actions can still activate app-defined behavior.
- General console replies stop after 250 ms of silence after the first reply, with
  a five-second total cap. Silent commands return an empty reply. Slow/continuous
  commands may yield partial output. PNG capture instead waits for its terminator
  for up to 30 seconds and checks the framing and checksum.
- Bridge logs are the bridge's retained history; serial has no historical buffer
  and collects new lines for at most two seconds. Neither is a durable log store.
- File tools handle UTF-8 text up to 1 MiB. App names are lowercase IDs, file paths
  are app-relative with forward slashes. Traversal, absolute/drive/UNC/ADS/device
  paths and escaping symlinks/junctions are refused. Hard-linked files are refused.
  Builds reject app trees with escaping links. These checks assume no other local
  process races to replace paths while an operation is running.
- API lookup inherits the SDK's `apidoc` parser; it is not a complete C parser.
  Search returns at most eight README sections and 60 API entries, with total
  match counts. Exact header resources remain available for complete context.
- The existing SDK install regression suite also builds a real RISC-V app and
  uploads it to a fake board. MCP's compiler adapter and simulator are stubs;
  real renderer output, hardware, and actual client UIs still need an end-to-end check.

Protocol reference: [MCP stdio transport](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
and [MCP tools](https://modelcontextprotocol.io/specification/2025-06-18/server/tools).
