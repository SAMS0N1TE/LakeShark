# TERMINAL

The serial console's commands, typed and read on the board itself.

| | |
|---|---|
| HOME group | SYSTEM |
| Radio | None |

## What it does

TERMINAL runs the same commands as the USB serial console, with the output
on the screen. Use it in the field when no laptop is plugged in: read the
error log with `crumb`, check memory with `heap`, or control an app from the
command line.

Type `help` to list the commands in your build.

## Radio

None. Some commands control radio apps, for example `sweep on` or
`sweep status`.

## How to use it

| Key | Touch | Action |
|---|---|---|
| Letters | KEYS, or tap the input line | Type a command |
| Enter | | Run the line |
| Up, Down | | Recall earlier commands (last 16) |
| Tab | | Complete the command name, or list the matches |
| Left, Right | Tap the top or bottom half of the text | Scroll the output back or forward |
| Backspace | | Delete a character |
| Esc | BACK | Clear the line; on an empty line, back to HOME |

The button bar:

- **BACK**: return to HOME.
- **KEYS**: open the touch keyboard. Its text runs as soon as you finish.
- **ERRORS**: runs `crumb log`, the saved error log.
- **HEAP**: shows memory.
- **CLEAR**: clears the screen. Typing `clear` or `cls` does the same.

ERRORS and HEAP are greyed out while a command is running.

## What the screen shows

- The output, newest at the bottom. Your commands appear after the
  `lakeshark>` prompt in green. Errors are red; warnings and unknown commands
  are yellow.
- A yellow "back N" tag on the frame while you are scrolled back.
- The input line with a cursor, or "running" with the command while one is
  in progress.
- A note line at the bottom, for example when a command is still running or
  no command matches a completion.

## Tips and limits

- One command runs at a time. A new line waits until the last one finishes.
- A command line can be up to 120 characters.
- The output keeps the last 8 KB; older lines drop off the top.
- The command runner needs some internal memory. If the screen says there is
  not enough, close a radio app and try again.
- A command keeps running if you leave TERMINAL.

## More guides

- [DIAG](DIAG.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
