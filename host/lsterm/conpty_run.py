"""Run a program inside a real Windows pseudo-console, type into it, capture the screen.

  python -I conpty_run.py --cols 120 --rows 32 --script "wait 3; type 2; wait 2; type \\x1d; wait 2" -- prog args...

The keyboard path of lssim --term and lsterm.py only runs in a real console
(pipes skip it); this is a console without a window. Steps: `wait <s>`,
`type <text>` (Python escapes allowed). Prints the final screen as text from
the console's own redraws, and whether the program exited by itself.
"""
import ctypes
import ctypes.wintypes as wt
import re
import sys
import threading
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
HPCON = ctypes.c_void_p


class COORD(ctypes.Structure):
    _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                ("hStdError", wt.HANDLE)]


class STARTUPINFOEXW(ctypes.Structure):
    _fields_ = [("StartupInfo", STARTUPINFOW), ("lpAttributeList", ctypes.c_void_p)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


def main():
    argv = sys.argv[1:]
    cols = int(argv[argv.index("--cols") + 1]) if "--cols" in argv else 120
    rows = int(argv[argv.index("--rows") + 1]) if "--rows" in argv else 32
    script = argv[argv.index("--script") + 1] if "--script" in argv else "wait 3"
    prog = argv[argv.index("--") + 1:]
    in_r, in_w, out_r, out_w = wt.HANDLE(), wt.HANDLE(), wt.HANDLE(), wt.HANDLE()
    k32.CreatePipe(ctypes.byref(in_r), ctypes.byref(in_w), None, 0)
    k32.CreatePipe(ctypes.byref(out_r), ctypes.byref(out_w), None, 0)
    hpc = HPCON()
    hr = k32.CreatePseudoConsole(COORD(cols, rows), in_r, out_w, 0, ctypes.byref(hpc))
    if hr:
        sys.exit("CreatePseudoConsole failed %x" % hr)
    size = ctypes.c_size_t()
    k32.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
    buf = ctypes.create_string_buffer(size.value)
    k32.InitializeProcThreadAttributeList(buf, 1, 0, ctypes.byref(size))
    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
    k32.UpdateProcThreadAttribute(buf, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, ctypes.sizeof(HPCON), None, None)
    si = STARTUPINFOEXW()
    si.StartupInfo.cb = ctypes.sizeof(STARTUPINFOEXW)
    # Null std handles, or a child of a parent whose own are pipes writes to those, not the console.
    si.StartupInfo.dwFlags = 0x00000100   # STARTF_USESTDHANDLES
    si.lpAttributeList = ctypes.cast(buf, ctypes.c_void_p)
    pi = PROCESS_INFORMATION()
    cmd = ctypes.create_unicode_buffer(" ".join('"%s"' % a if " " in a else a for a in prog))
    EXTENDED_STARTUPINFO_PRESENT = 0x00080000
    if not k32.CreateProcessW(None, cmd, None, None, False, EXTENDED_STARTUPINFO_PRESENT, None, None,
                              ctypes.byref(si), ctypes.byref(pi)):
        sys.exit("CreateProcess failed %d" % ctypes.get_last_error())
    out = bytearray()

    def reader():
        chunk = ctypes.create_string_buffer(65536)
        n = wt.DWORD()
        while k32.ReadFile(out_r, chunk, 65536, ctypes.byref(n), None) and n.value:
            out.extend(chunk.raw[:n.value])

    threading.Thread(target=reader, daemon=True).start()
    for step in [s.strip() for s in script.split(";") if s.strip()]:
        verb, _, arg = step.partition(" ")
        if verb == "wait":
            time.sleep(float(arg))
        elif verb == "type":
            data = arg.encode().decode("unicode_escape").encode("latin-1")
            k32.WriteFile(in_w, data, len(data), ctypes.byref(wt.DWORD()), None)
    exited = k32.WaitForSingleObject(pi.hProcess, 3000) == 0
    if not exited:
        k32.TerminateProcess(pi.hProcess, 1)
    k32.ClosePseudoConsole(hpc)
    time.sleep(0.3)
    text = out.decode("utf-8", "replace")
    # The console redraws with cursor moves and colours; keep the text, rows by CUP.
    screen = {}
    r = c = 0
    for m in re.finditer(r"\x1b\[(\d*);?(\d*)H|\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07|([^\x1b])", text):
        if m.group(0).endswith("H") and m.group(0).startswith("\x1b["):
            r = int(m.group(1) or 1) - 1
            c = int(m.group(2) or 1) - 1
        elif m.group(3) is not None:
            ch = m.group(3)
            if ch == "\r":
                c = 0
            elif ch == "\n":
                r += 1
            else:
                screen[(r, c)] = ch
                c += 1
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for row in range(rows):
        print("".join(screen.get((row, x), " ") for x in range(cols)).rstrip())
    print("bytes=%d exited_by_itself=%s" % (len(out), exited))


if __name__ == "__main__":
    main()
