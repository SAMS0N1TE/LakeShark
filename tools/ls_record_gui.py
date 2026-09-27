"""A window for recording the board's screen as it is.

    pythonw tools/ls_record_gui.py

It records whatever the board is showing when you press RECORD: it sends no
commands that change screens. Each take gets a new time-stamped name, and
ls_record.py adds _2, _3... if that name is ever taken, so nothing written
before is replaced. The board ignores the console while it records, so a
take runs for the length chosen.
"""
import datetime
import os
import queue
import re
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ls_console  # noqa: E402

RECORD = os.path.join(HERE, "ls_record.py")
SERIAL = "5C84301528"
DEFAULT_DIR = os.path.join(os.path.expanduser("~"), "Videos", "LakeShark")
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class App:
    def __init__(self, root):
        self.root = root
        self.proc = None
        self.lines = queue.Queue()
        self.deadline = None
        self.seconds = 0
        root.title("LakeShark screen recorder")
        root.resizable(False, False)
        pad = {"padx": 8, "pady": 4}

        board = ttk.LabelFrame(root, text="Board")
        board.grid(row=0, column=0, sticky="ew", **pad)
        self.port_var = tk.StringVar(value="looking...")
        ttk.Label(board, textvariable=self.port_var, width=34).grid(row=0, column=0, sticky="w", **pad)
        ttk.Button(board, text="Find again", command=self.find_port).grid(row=0, column=1, **pad)

        take = ttk.LabelFrame(root, text="Take")
        take.grid(row=1, column=0, sticky="ew", **pad)
        ttk.Label(take, text="Length (seconds)").grid(row=0, column=0, sticky="w", **pad)
        self.len_var = tk.IntVar(value=15)
        ttk.Spinbox(take, from_=3, to=300, textvariable=self.len_var, width=6).grid(row=0, column=1, sticky="w", **pad)
        ttk.Label(take, text="Size").grid(row=1, column=0, sticky="w", **pad)
        self.scale_var = tk.StringVar(value="2x, sharp")
        ttk.Combobox(take, textvariable=self.scale_var, values=["2x, sharp", "1x, smaller file"],
                     state="readonly", width=16).grid(row=1, column=1, sticky="w", **pad)
        self.gif_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(take, text="Also make a GIF", variable=self.gif_var).grid(row=2, column=0, columnspan=2,
                                                                                  sticky="w", **pad)
        self.keep_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(take, text="Keep the raw recording (.lsrec) to re-encode later",
                        variable=self.keep_var).grid(row=3, column=0, columnspan=2, sticky="w", **pad)

        where = ttk.LabelFrame(root, text="Save to")
        where.grid(row=2, column=0, sticky="ew", **pad)
        self.dir_var = tk.StringVar(value=DEFAULT_DIR)
        ttk.Entry(where, textvariable=self.dir_var, width=40).grid(row=0, column=0, **pad)
        ttk.Button(where, text="Browse", command=self.browse).grid(row=0, column=1, **pad)
        ttk.Label(where, text="Name").grid(row=1, column=0, sticky="w", **pad)
        self.name_var = tk.StringVar(value="lakeshark")
        ttk.Entry(where, textvariable=self.name_var, width=20).grid(row=1, column=0, sticky="e", **pad)

        run = ttk.Frame(root)
        run.grid(row=3, column=0, sticky="ew", **pad)
        self.rec_btn = ttk.Button(run, text="RECORD", command=self.start)
        self.rec_btn.grid(row=0, column=0, **pad)
        self.status_var = tk.StringVar(value="Ready. The board records whatever it is showing.")
        ttk.Label(run, textvariable=self.status_var, width=44).grid(row=0, column=1, sticky="w", **pad)
        self.bar = ttk.Progressbar(run, length=420, mode="determinate")
        self.bar.grid(row=1, column=0, columnspan=2, **pad)

        done = ttk.LabelFrame(root, text="Written this session (double-click to open)")
        done.grid(row=4, column=0, sticky="ew", **pad)
        self.files = tk.Listbox(done, height=6, width=62)
        self.files.grid(row=0, column=0, columnspan=2, **pad)
        self.files.bind("<Double-Button-1>", self.open_selected)
        ttk.Button(done, text="Open folder", command=self.open_folder).grid(row=1, column=0, sticky="w", **pad)

        self.log = tk.Text(root, height=5, width=62, state="disabled", font=("Consolas", 8))
        self.log.grid(row=5, column=0, **pad)

        self.find_port()
        self.root.after(100, self.poll)

    def find_port(self):
        port = ls_console.find_port(SERIAL)
        self.port = port
        self.port_var.set("T-Display-P4 on %s" % port if port else "not found: plug in the board's USB")

    def browse(self):
        d = filedialog.askdirectory(initialdir=self.dir_var.get() or DEFAULT_DIR)
        if d:
            self.dir_var.set(d)

    def say(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def start(self):
        if self.proc:
            return
        self.find_port()
        if not self.port:
            messagebox.showerror("Recorder", "The board is not connected.")
            return
        try:
            seconds = int(self.len_var.get())
        except (tk.TclError, ValueError):
            seconds = 0
        if not 3 <= seconds <= 300:
            messagebox.showerror("Recorder", "Length is 3 to 300 seconds.")
            return
        folder = self.dir_var.get().strip() or DEFAULT_DIR
        os.makedirs(folder, exist_ok=True)
        name = re.sub(r"[^\w-]+", "_", self.name_var.get().strip()) or "lakeshark"
        stem = os.path.join(folder, "%s_%s" % (name, datetime.datetime.now().strftime("%Y%m%d_%H%M%S")))
        cmd = [sys.executable.replace("pythonw.exe", "python.exe"), "-u", RECORD, "-o", stem + ".mp4",
               "--seconds", str(seconds), "--port", self.port,
               "--scale", "1" if self.scale_var.get().startswith("1x") else "2"]
        if self.gif_var.get():
            cmd.append("--gif")
        if self.keep_var.get():
            cmd += ["--keep", stem + ".lsrec"]
        self.seconds = seconds
        self.deadline = None
        self.bar.configure(mode="determinate", maximum=seconds, value=0)
        self.status_var.set("Starting...")
        self.rec_btn.configure(state="disabled")
        self.say("take: %s" % os.path.basename(stem))
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding="utf-8", errors="replace", creationflags=NO_WINDOW)
        threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()

    def read(self, proc):
        for line in proc.stdout:
            self.lines.put(line.rstrip())
        self.lines.put(("exit", proc.wait()))

    def poll(self):
        now = datetime.datetime.now().timestamp()
        while not self.lines.empty():
            item = self.lines.get()
            if isinstance(item, tuple):
                self.finished(item[1])
                continue
            self.say(item)
            if item.startswith("recording "):
                self.deadline = now + self.seconds
                self.status_var.set("Recording...")
            elif item.startswith("scene "):
                self.deadline = None
                self.bar.configure(mode="indeterminate")
                self.bar.start(15)
                self.status_var.set("Encoding...")
            elif item.startswith("wrote "):
                path = item[6:].split(":", 2)
                path = ":".join(path[:2]) if len(path) > 2 else path[0]
                self.files.insert("end", path)
        if self.deadline:
            left = max(0.0, self.deadline - now)
            self.bar.configure(value=self.seconds - left)
            self.status_var.set("Recording... %d s left" % round(left))
        self.root.after(100, self.poll)

    def finished(self, code):
        self.bar.stop()
        self.bar.configure(mode="determinate", value=0)
        self.proc = None
        self.deadline = None
        self.rec_btn.configure(state="normal")
        self.status_var.set("Done." if code == 0 else "Failed: see the log below.")

    def open_selected(self, _event=None):
        sel = self.files.curselection()
        if sel:
            os.startfile(self.files.get(sel[0]))

    def open_folder(self):
        folder = self.dir_var.get().strip() or DEFAULT_DIR
        os.makedirs(folder, exist_ok=True)
        os.startfile(folder)


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
