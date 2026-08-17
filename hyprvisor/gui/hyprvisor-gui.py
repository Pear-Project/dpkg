#!/usr/bin/env python3
"""hyprvisor-gui — dark GTK front-end for the hyprvisor CLI (Debian/Ubuntu edition).

Detects the current GPU, shows the driver package(s) ubuntu-drivers reports
for it (recommended one starred), and installs the pick via pkexec apt-get.
No AUR/pacman, no per-generation package database, no Simulate window --
all of that only made sense when hyprvisor carried its own Arch/AUR package
recipe book. Here it's a thin front-end over `ubuntu-drivers`/apt.

Requires: python-gobject (GTK 3), the hyprvisor binary (built or in PATH).
"""

import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk, GLib

import json
import os
import re
import shutil
import subprocess
import threading

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def strip_ansi(text):
    return _ANSI_RE.sub("", text)


def find_binary():
    p = shutil.which("hyprvisor")
    if p:
        return p
    local = os.path.join(os.path.dirname(os.path.abspath(__file__)), "/", "usr", "bin", "hyprvisor")
    local = os.path.abspath(local)
    if os.path.isfile(local):
        return local
    return None


HYPRVISOR_BIN = find_binary()


def run_hyprvisor_json(args):
    if not HYPRVISOR_BIN:
        raise FileNotFoundError(
            "hyprvisor binary not found (build it first: cmake --build build/)")
    proc = subprocess.run([HYPRVISOR_BIN] + args + ["--json"],
                           capture_output=True, text=True, timeout=30)
    out = proc.stdout.strip()
    if not out:
        raise RuntimeError(proc.stderr.strip() or "hyprvisor produced no output")
    return json.loads(out)


# ── main panel ───────────────────────────────────────────────────────────

class DriverPanel(Gtk.Box):
    def __init__(self):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.set_border_width(12)
        self.gpu = None
        self.drivers = []

        self.gpu_label = Gtk.Label(xalign=0)
        self.gpu_label.set_markup("<span size='large'><b>No GPU detected yet</b></span>")
        self.gpu_label.set_line_wrap(True)
        self.pack_start(self.gpu_label, False, False, 0)

        self.sub_label = Gtk.Label(xalign=0)
        self.sub_label.get_style_context().add_class("dim-label")
        self.pack_start(self.sub_label, False, False, 0)

        self.store = Gtk.ListStore(str, str, str, str)
        self.tree = Gtk.TreeView(model=self.store)
        for i, title in enumerate(["Package", "Recommended", "Status", "Description"]):
            renderer = Gtk.CellRendererText()
            if title == "Description":
                renderer.set_property("ellipsize", 3)  # Pango.EllipsizeMode.END
            col = Gtk.TreeViewColumn(title, renderer, text=i)
            col.set_resizable(True)
            if title == "Description":
                col.set_expand(True)
            self.tree.append_column(col)
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroll.set_min_content_height(240)
        scroll.add(self.tree)
        self.pack_start(scroll, True, True, 0)

        btn_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.install_btn = Gtk.Button(label="Install Recommended")
        self.install_btn.get_style_context().add_class("suggested-action")
        self.install_btn.connect("clicked", self.on_install_clicked)
        btn_row.pack_start(self.install_btn, False, False, 0)
        self.status_label = Gtk.Label(xalign=0)
        btn_row.pack_start(self.status_label, True, True, 0)
        self.pack_start(btn_row, False, False, 0)

        self.log_buf = Gtk.TextBuffer()
        log_view = Gtk.TextView(buffer=self.log_buf)
        log_view.set_editable(False)
        log_view.set_monospace(True)
        log_scroll = Gtk.ScrolledWindow()
        log_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        log_scroll.set_min_content_height(110)
        log_scroll.add(log_view)
        self.pack_start(log_scroll, False, False, 0)

    def set_gpu(self, gpu, vm_name=""):
        self.gpu = gpu
        self.gpu_label.set_markup(f"<span size='large'><b>{GLib.markup_escape_text(gpu['name'])}</b></span>")
        sub = f"Vendor: {gpu.get('vendorName', '?')}"
        if gpu.get("pciAddr"):
            sub += f"   PCI: {gpu['pciAddr']}   IDs: {gpu.get('vendorId', '')}:{gpu.get('deviceId', '')}"
        if vm_name:
            sub += f"   [inside {vm_name}]"
        self.sub_label.set_text(sub)

        self.drivers = gpu.get("drivers", [])
        self.refresh_table()

    def refresh_table(self):
        self.store.clear()
        for d in self.drivers:
            pkg = d["package"] + (" *" if d["recommended"] else "")
            rec = "yes" if d["recommended"] else ""
            status = "installed" if d["installed"] else "not installed"
            self.store.append([pkg, rec, status, d["description"]])

        if not self.drivers:
            self.status_label.set_text("Already fully supported by mesa/in-kernel driver.")
            self.install_btn.set_sensitive(False)
            return

        missing = [d for d in self.drivers if not d["installed"] and d["recommended"]]
        if not missing:
            self.status_label.set_text("Recommended driver already installed.")
            self.install_btn.set_sensitive(False)
        else:
            self.status_label.set_text(f"Recommended: {missing[0]['package']}")
            self.install_btn.set_sensitive(True)

    def log(self, text):
        text = strip_ansi(text)
        def do():
            end = self.log_buf.get_end_iter()
            self.log_buf.insert(end, text + "\n")
        GLib.idle_add(do)

    def on_install_clicked(self, _btn):
        dialog = Gtk.MessageDialog(
            transient_for=self.get_toplevel(), modal=True,
            message_type=Gtk.MessageType.QUESTION, buttons=Gtk.ButtonsType.YES_NO,
            text="Install the recommended driver package?")
        dialog.format_secondary_text("This runs: pkexec apt-get install -y <package>")
        resp = dialog.run()
        dialog.destroy()
        if resp != Gtk.ResponseType.YES:
            return

        self.install_btn.set_sensitive(False)
        threading.Thread(target=self._do_install, daemon=True).start()

    def _do_install(self):
        self.log(f"$ {HYPRVISOR_BIN} --install --noconfirm")
        try:
            proc = subprocess.Popen([HYPRVISOR_BIN, "--install", "--noconfirm"],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in proc.stdout:
                self.log(line.rstrip())
            proc.wait()
            self.log(f"[hyprvisor exited with code {proc.returncode}]")
        except Exception as e:
            self.log(f"[error] {e}")
        GLib.idle_add(self._install_finished)

    def _install_finished(self):
        try:
            data = run_hyprvisor_json(["--list"])
            if data.get("gpus"):
                self.set_gpu(data["gpus"][0], data.get("vmName", ""))
        except Exception as e:
            self.log(f"[error refreshing status] {e}")
        self.install_btn.set_sensitive(True)


# ── main window ──────────────────────────────────────────────────────────

class MainWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="hyprvisor")
        self.set_default_size(700, 520)
        self.connect("destroy", Gtk.main_quit)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.add(box)

        header = Gtk.HeaderBar(title="hyprvisor", subtitle="GPU driver manager")
        header.set_show_close_button(True)
        refresh_btn = Gtk.Button.new_from_icon_name("view-refresh-symbolic", Gtk.IconSize.BUTTON)
        refresh_btn.set_tooltip_text("Re-detect GPU")
        refresh_btn.connect("clicked", lambda _b: self.load_gpu())
        header.pack_start(refresh_btn)
        clean_btn = Gtk.Button(label="Clean All…")
        clean_btn.get_style_context().add_class("destructive-action")
        clean_btn.set_tooltip_text("Purge every installed NVIDIA driver package for a fresh start")
        clean_btn.connect("clicked", self.on_clean)
        header.pack_end(clean_btn)
        self.set_titlebar(header)

        self.panel = DriverPanel()
        box.pack_start(self.panel, True, True, 0)

        self.load_gpu()

    def load_gpu(self):
        try:
            data = run_hyprvisor_json(["--list"])
        except Exception as e:
            self.panel.gpu_label.set_markup(f"<span foreground='#ff6b6b'><b>Error: {GLib.markup_escape_text(str(e))}</b></span>")
            return
        if not data.get("gpus"):
            self.panel.gpu_label.set_markup("<span foreground='#ff6b6b'><b>No GPU detected</b></span>")
            return
        self.panel.set_gpu(data["gpus"][0], data.get("vmName", ""))

    def on_clean(self, _btn):
        if not HYPRVISOR_BIN:
            return
        try:
            # Dry-run: --clean without --noconfirm lists what it would purge
            # and waits on a y/N prompt; feed "n" so nothing is touched, just
            # to read the real package list before asking our own dialog.
            proc = subprocess.run([HYPRVISOR_BIN, "--clean"], input="n\n",
                                   capture_output=True, text=True, timeout=20)
        except Exception as e:
            self.panel.log(f"[error] {e}")
            return

        out = strip_ansi(proc.stdout)
        pkgs = [line.strip()[2:].strip() for line in out.splitlines()
                if line.strip().startswith("- ")]
        if not pkgs:
            dialog = Gtk.MessageDialog(
                transient_for=self, modal=True, message_type=Gtk.MessageType.INFO,
                buttons=Gtk.ButtonsType.OK, text="Nothing to clean")
            dialog.format_secondary_text("No NVIDIA driver packages installed.")
            dialog.run()
            dialog.destroy()
            return

        dialog = Gtk.MessageDialog(
            transient_for=self, modal=True, message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.YES_NO,
            text="Purge ALL NVIDIA driver packages?")
        dialog.format_secondary_text("\n".join(pkgs))
        resp = dialog.run()
        dialog.destroy()
        if resp != Gtk.ResponseType.YES:
            return

        threading.Thread(target=self._do_clean, daemon=True).start()

    def _do_clean(self):
        self.panel.log(f"$ {HYPRVISOR_BIN} --clean --noconfirm")
        try:
            proc = subprocess.Popen([HYPRVISOR_BIN, "--clean", "--noconfirm"],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in proc.stdout:
                self.panel.log(line.rstrip())
            proc.wait()
            self.panel.log(f"[hyprvisor exited with code {proc.returncode}]")
        except Exception as e:
            self.panel.log(f"[error] {e}")
        GLib.idle_add(self.load_gpu)


DARK_CSS = b"""
window { background-color: #1e1f22; color: #e6e6e6; }
label { color: #e6e6e6; }
label.dim-label { color: #9a9a9a; }
treeview, textview text { background-color: #26272b; color: #e6e6e6; }
treeview:selected { background-color: #3a6ea5; }
headerbar { background-color: #26272b; color: #e6e6e6; }
button.suggested-action { background-color: #3a6ea5; color: white; }
button.destructive-action { background-color: #a53a3a; color: white; }
"""


def apply_dark_theme():
    settings = Gtk.Settings.get_default()
    if settings:
        settings.set_property("gtk-application-prefer-dark-theme", True)
    provider = Gtk.CssProvider()
    provider.load_from_data(DARK_CSS)
    Gtk.StyleContext.add_provider_for_screen(
        Gdk.Screen.get_default(), provider,
        Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)


def main():
    apply_dark_theme()
    win = MainWindow()
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
