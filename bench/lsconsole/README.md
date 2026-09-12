# lsconsole - one console, two operators

The board has one serial port and only one process can hold it. A GUI for you
and a command line for an agent cannot both open it: whoever gets there first
wins and the other silently sees a dead board. So a **broker** owns the port
and both of us are clients of it.

```
  GUI  ─┐
        ├─►  broker (owns COM9, keeps a rolling log)  ─►  board
  CLI  ─┘
```

## Your side

```powershell
./bench/lsconsole.ps1 gui
```

Buttons grouped by what they do, presets that run a sequence, a free-text box,
and a live console. Hovering a button explains it. Everything is one tap.

The two buttons that matter when we are both working: **RELEASE PORT** before
a flash, **REATTACH** after. `esptool` needs exclusive access and will fail
with the port held.

## My side

```powershell
./bench/lsconsole.ps1 send "status"
./bench/lsconsole.ps1 send "tui key f1" -Wait 2
./bench/lsconsole.ps1 tail -n 100        # read without sending
./bench/lsconsole.ps1 pause              # before I flash
./bench/lsconsole.ps1 resume
./bench/lsconsole.ps1 list               # every button and preset
```

Whichever of us asks first starts the broker; neither of us owns it.

## The shared part

`commands.json` is the point of the whole thing. Both clients read it, so a
button you press and a command I send are the same string from the same file.
Add a button there and it appears in your GUI and in my `list` at once.

**Every command in it was read out of the board's own `help`** on 2026-09-09,
firmware `1.0.3-ge423d9845154-dirty`, T-Display-P4 `5C84301528`. The first
version of this file was written from guesses and about a third of it was
wrong - commands that did not exist, and real ones like `mode`, `freq`,
`gain`, `spec`, `beep`, `keys` and `display` missing entirely. If you add one,
run `help` and copy the real syntax.

## Serial rules it obeys, none of them optional

- **DTR and RTS are set false before the port opens.** The CH343 auto-reset is
  hardware: assert either line and the board reboots. PuTTY and `idf.py
  monitor` both do, which is why they eat captures.
- **A bare newline goes first.** The first command after opening is reliably
  swallowed by leftover bytes in the firmware's line buffer.
- **The board is found by its CH343 serial** from `bench/configs.json`, never
  by COM number. COM numbers move; naming a port instead of a board is how the
  LCD once received the nano's image.
- The reader drains continuously rather than only around a command, because
  the console is shared with the log and a chatty subsystem will overrun a
  buffer that is only read in bursts.

## Relationship to bench/console.ps1

`console.ps1` still works and is still right for a single one-shot command
with nothing else attached. Use `lsconsole` when the GUI is open, or when two
things need the board in one session.

## Timestamps and round-trip time

Every line carries the moment it ARRIVED, and every command reports how long
it took:

```
[11:45:07.095] >> version
[11:45:07.215] lakeshark> version
[11:45:07.215] LakeShark 1.0.3-ge423d9845154-dirty  board=T-Display-P4 ...
-- 1259 ms
```

The time is stamped in the reader, not when a client formats it - the gap
between the board emitting a line and a GUI drawing it is exactly the interval
being measured. Meshtastic's device log does the same thing for the same
reason.

This is not decoration. It found a real bug within a minute of being added:
every command was silently burning the full 15-second prompt timeout, because
the prompt match assumed an unterminated prompt and this firmware emits a
terminated one. A `version` whose reply arrived in 120 ms was taking 15,280.
Nobody could see that until a round-trip time was printed. See .

**For anything consuming this as an API**, the TCP reply carries both forms:

```json
{ "ok": true,
  "lines":  ["[11:45:07.215] lakeshark> version", "..."],
  "events": [{"t": 1788970-.., "iso": "2026-09-09T11:45:07.215-04:00",
              "text": "lakeshark> version"}],
  "elapsed_ms": 1259,
  "prompt": true }
```

`lines` is display-ready and keeps every existing caller working. `events` is
structured - epoch seconds plus ISO-8601 with offset - so a consumer can sort,
diff or re-render without parsing a prefix.

## Restarting the broker after editing lsconsole.py

The broker is a long-lived process holding the port. A client's `resume` will
reattach to the OLD one, still running the old code, so an edit appears to do
nothing. Kill every `serve` process, not just whatever is listening on 8731 -
killing the listener alone can leave a second one holding COM9 with no
listener, and then nothing can open the port at all:

```powershell
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -match 'lsconsole\.py serve' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

The next client request starts a fresh one. Leave any `gui` process alone.

## Known rough edges

- The reply is not separated from the log, because on one wire it cannot be.
  You get everything that arrived in the window.
- `wait` is now a MINIMUM settle, not the whole budget. The broker waits for
  a line that is exactly the prompt, with `wait` (or 15 s, whichever is
  larger) as a backstop. A command that legitimately runs long - `lora rx 30`
  - still needs a `-Wait` past its own duration.
- The broker does not survive a reboot of the host, and restarting it after
  editing `lsconsole.py` needs the old one killed - it is a plain Python
  process, `taskkill /F /IM python.exe` is heavier than needed but works.
