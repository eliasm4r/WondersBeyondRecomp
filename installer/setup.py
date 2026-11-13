import argparse
import ctypes
import os
import queue
import subprocess
import sys
import threading
import tkinter
import traceback
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import steps
from texts import TEXTS

HERE = Path(__file__).resolve().parent


def folder_problem(folder, texts):
    if not folder or not Path(folder).is_absolute() or not folder.isascii() or any(sign in folder[2:] for sign in '<>:"|?*%&^!;'):
        return texts["plain_folder"]
    if len(folder) > steps.LONGEST_FOLDER:
        return texts["long_folder"]
    return ""


class Window:
    def __init__(self):
        self.texts = TEXTS
        self.install = None
        self.events = queue.Queue()
        self.executable = ""
        t = self.texts
        for name, folder in (("TCL_LIBRARY", "tcl8.6"), ("TK_LIBRARY", "tk8.6")):
            os.environ[name] = str(HERE.parent / "python" / "tcl" / folder)
        self.root = tkinter.Tk()
        self.root.title(t["window"])
        self.root.resizable(False, False)
        try:
            self.root.iconbitmap(str(HERE / "game.ico"))
        except tkinter.TclError:
            pass
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        frame = ttk.Frame(self.root, padding=16)
        frame.grid(sticky="nsew")
        ttk.Label(frame, text=t["title"], font=("Segoe UI", 13, "bold")).grid(row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(frame, text=t["intro"].format(size=steps.NEEDED_GIGABYTES), wraplength=520, justify="left").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(6, 14))
        self.disc = tkinter.StringVar()
        self.folder = tkinter.StringVar(value=steps.default_folder())
        self.shortcut = tkinter.BooleanVar(value=True)
        self.status = tkinter.StringVar()
        ttk.Label(frame, text=t["disc"]).grid(row=2, column=0, columnspan=3, sticky="w")
        self.disc_entry = ttk.Entry(frame, textvariable=self.disc, width=64)
        self.disc_entry.grid(row=3, column=0, columnspan=2, sticky="we", pady=(2, 10))
        self.disc_button = ttk.Button(frame, text=t["browse"], command=self.choose_disc)
        self.disc_button.grid(row=3, column=2, sticky="e", padx=(8, 0), pady=(2, 10))
        ttk.Label(frame, text=t["folder"]).grid(row=4, column=0, columnspan=3, sticky="w")
        self.folder_entry = ttk.Entry(frame, textvariable=self.folder, width=64)
        self.folder_entry.grid(row=5, column=0, columnspan=2, sticky="we", pady=(2, 10))
        self.folder_button = ttk.Button(frame, text=t["browse"], command=self.choose_folder)
        self.folder_button.grid(row=5, column=2, sticky="e", padx=(8, 0), pady=(2, 10))
        self.shortcut_box = ttk.Checkbutton(frame, text=t["shortcut"], variable=self.shortcut)
        self.shortcut_box.grid(row=6, column=0, columnspan=3, sticky="w")
        self.bar = ttk.Progressbar(frame, maximum=1000)
        self.bar.grid(row=7, column=0, columnspan=3, sticky="we", pady=(16, 4))
        ttk.Label(frame, textvariable=self.status, wraplength=520, justify="left").grid(row=8, column=0, columnspan=3, sticky="w")
        buttons = ttk.Frame(frame)
        buttons.grid(row=9, column=0, columnspan=3, sticky="e", pady=(14, 0))
        self.main_button = ttk.Button(buttons, text=t["install"], command=self.start, default="active")
        self.main_button.grid(row=0, column=0, padx=(0, 8))
        self.close_button = ttk.Button(buttons, text=t["close"], command=self.close)
        self.close_button.grid(row=0, column=1)
        frame.columnconfigure(0, weight=1)
        self.folder.trace_add("write", lambda *change: self.refresh())
        self.refresh()
        self.root.after(100, self.poll)

    def inputs(self, state):
        for widget in (self.disc_entry, self.disc_button, self.folder_entry, self.folder_button, self.shortcut_box):
            widget.configure(state=state)

    def refresh(self):
        t = self.texts
        self.update = steps.game_extracted(self.folder.get().strip().strip('"'))
        for widget in (self.disc_entry, self.disc_button):
            widget.configure(state="disabled" if self.update else "normal")
        self.main_button.configure(text=t["update" if self.update else "install"])
        self.status.set(t["already"] if self.update else "")

    def choose_disc(self):
        t = self.texts
        path = filedialog.askopenfilename(parent=self.root, title=t["disc_title"],
                                          filetypes=[(t["disc_types"], "*.iso *.rvz *.wbfs *.wia *.ciso *.gcz *.nfs"), (t["all_files"], "*.*")])
        if path:
            self.disc.set(str(Path(path)))

    def choose_folder(self):
        path = filedialog.askdirectory(parent=self.root, title=self.texts["folder_title"])
        if not path:
            return
        folder = Path(path)
        if folder.name != steps.FOLDER_NAME and not steps.game_extracted(folder):
            folder = folder / steps.FOLDER_NAME
        self.folder.set(str(folder))

    def refuse(self, message):
        messagebox.showwarning(self.texts["window"], message, parent=self.root)

    def start(self):
        t = self.texts
        folder = self.folder.get().strip().strip('"')
        disc = self.disc.get().strip().strip('"')
        problem = folder_problem(folder, t)
        if problem:
            return self.refuse(problem)
        if not steps.game_extracted(folder):
            if not disc:
                return self.refuse(t["need_disc"])
            if not Path(disc).is_file():
                return self.refuse(t["missing_disc"].format(path=disc))
        free = steps.free_gigabytes(folder)
        if free < steps.NEEDED_GIGABYTES and not (Path(folder) / steps.EXECUTABLE).is_file():
            return self.refuse(t["low_space"].format(free=int(free), size=steps.NEEDED_GIGABYTES))
        self.inputs("disabled")
        self.main_button.configure(state="disabled")
        self.close_button.configure(text=t["cancel"])
        self.bar.configure(value=0)
        self.install = steps.Install(folder, disc, HERE.parent, self.shortcut.get(), t, lambda kind, value: self.events.put((kind, value)))
        threading.Thread(target=self.install.execute, daemon=True).start()

    def poll(self):
        t = self.texts
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "step":
                    self.status.set(value + "...")
                elif kind == "progress":
                    self.bar.configure(value=int(value * 1000))
                elif kind == "done":
                    self.executable = value
                    self.install = None
                    self.status.set(t["done_update" if self.update else "done_shortcut" if self.shortcut.get() else "done"])
                    self.main_button.configure(text=t["play"], command=self.play, state="normal")
                    self.close_button.configure(text=t["close"])
                else:
                    log = Path(self.install.root) / "logs" / "install.log"
                    self.install = None
                    self.bar.configure(value=0)
                    self.inputs("normal")
                    self.main_button.configure(state="normal")
                    self.close_button.configure(text=t["close"])
                    self.refresh()
                    if kind == "stopped":
                        self.status.set(t["cancelled"])
                    else:
                        self.status.set(t["failed"].format(step=value[0]))
                        messagebox.showerror(t["window"], value[1][:1500] + "\n\n" + t["log"].format(path=log), parent=self.root)
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def play(self):
        subprocess.Popen([self.executable], cwd=str(Path(self.executable).parent))
        self.root.destroy()

    def close(self):
        if self.install:
            if messagebox.askyesno(self.texts["window"], self.texts["confirm_cancel"], parent=self.root):
                install = self.install
                if install:
                    threading.Thread(target=install.cancel, daemon=True).start()
            return
        self.root.destroy()


def prepare(disc):
    t = TEXTS
    folder = str(HERE.parent)
    state = {"step": "", "shown": -1.0, "result": 1}

    def say(line):
        if sys.stdout:
            print(line, flush=True)

    def report(kind, value):
        if kind == "step":
            state["step"] = value
            state["shown"] = -1.0
        elif kind == "progress" and (value - state["shown"] >= 0.005 or state["shown"] < 0 or value >= 1):
            state["shown"] = value
            say(f"progress {value:.3f} {state['step']}")
        elif kind == "done":
            state["result"] = 0
        elif kind == "failed":
            say(t["failed"].format(step=value[0]))
            say(value[1])
            say(t["log"].format(path=Path(folder) / "logs" / "install.log"))

    if sys.stdout:
        sys.stdout.reconfigure(encoding="utf-8")
    problem = folder_problem(folder, t)
    if not problem and not steps.game_extracted(folder) and not Path(disc).is_file():
        problem = t["missing_disc"].format(path=disc)
    free = steps.free_gigabytes(folder)
    if not problem and free < steps.NEEDED_GIGABYTES and not (Path(folder) / steps.EXECUTABLE).is_file():
        problem = t["low_space"].format(free=int(free), size=steps.NEEDED_GIGABYTES)
    if problem:
        say(problem)
        return 1
    steps.Install(folder, disc, HERE.parent, False, t, report, menu=False).execute()
    return state["result"]


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--game", default="")
    arguments, _ = parser.parse_known_args()
    if arguments.prepare:
        return prepare(arguments.game)
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("WondersBeyondRecomp.Setup")
    except (AttributeError, OSError):
        pass
    try:
        Window().root.mainloop()
    except Exception:
        report = traceback.format_exc()
        try:
            (HERE.parent / "setup-error.txt").write_text(report, encoding="utf-8")
        except OSError:
            pass
        ctypes.windll.user32.MessageBoxW(None, report[-1500:], "Setup", 0x10)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
