#!/usr/bin/env python3
"""
Clean Mic GUI - GTK4/Libadwaita application for controlling Clean Mic.
"""

import sys
import os
import shutil
import json
import subprocess
import threading
import time
import queue
from pathlib import Path

# Ensure runtime environment variables are populated when launched in minimal environments (e.g. gio launch)
uid = os.getuid()
runtime_dir = f"/run/user/{uid}"
if "XDG_RUNTIME_DIR" not in os.environ and os.path.isdir(runtime_dir):
    os.environ["XDG_RUNTIME_DIR"] = runtime_dir

if "WAYLAND_DISPLAY" not in os.environ and os.path.isdir(runtime_dir):
    wayland_sockets = sorted([p for p in Path(runtime_dir).glob("wayland-[0-9]*") if not p.name.endswith(".lock") and p.is_socket()])
    if wayland_sockets:
        os.environ["WAYLAND_DISPLAY"] = wayland_sockets[-1].name

if "DISPLAY" not in os.environ and os.path.isdir("/tmp/.X11-unix"):
    x_sockets = sorted([p for p in Path("/tmp/.X11-unix").glob("X[0-9]*") if not p.name.endswith(".lock") and p.is_socket()])
    if x_sockets:
        os.environ["DISPLAY"] = ":" + x_sockets[-1].name[1:]

if "DBUS_SESSION_BUS_ADDRESS" not in os.environ and os.path.exists(f"{runtime_dir}/bus"):
    os.environ["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={runtime_dir}/bus"

local_bin = str(Path.home() / ".local" / "bin")
current_path = os.environ.get("PATH", "")
if local_bin not in current_path.split(":"):
    os.environ["PATH"] = f"{local_bin}:{current_path}" if current_path else local_bin

if "XDG_DATA_DIRS" not in os.environ:
    os.environ["XDG_DATA_DIRS"] = f"{Path.home()}/.local/share:/usr/local/share:/usr/share"

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
from gi.repository import Gtk, Adw, GLib, Gio


def get_cleanmic_binary() -> str | None:
    """Resolve cleanmic binary path following brief priority."""
    # 1. CLEANMIC_BIN environment variable
    env_bin = os.environ.get("CLEANMIC_BIN")
    if env_bin:
        p = Path(env_bin).expanduser()
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
        which_env = shutil.which(env_bin)
        if which_env:
            return which_env

    # 2. PATH cleanmic
    which_path = shutil.which("cleanmic")
    if which_path:
        return which_path

    # 3. ../bin/cleanmic relative to this file
    repo_bin = Path(__file__).resolve().parent.parent / "bin" / "cleanmic"
    if repo_bin.is_file() and os.access(repo_bin, os.X_OK):
        return str(repo_bin)

    # 4. ~/.local/bin/cleanmic
    local_bin = Path.home() / ".local" / "bin" / "cleanmic"
    if local_bin.is_file() and os.access(local_bin, os.X_OK):
        return str(local_bin)

    return None


def run_cli_sync(args: list[str], timeout: float = 6.0) -> tuple[int, str, str]:
    """Execute cleanmic CLI with given arguments synchronously."""
    bin_path = get_cleanmic_binary()
    if not bin_path:
        return -1, "", "cleanmic binary not found"
    try:
        res = subprocess.run(
            [bin_path] + args,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        return res.returncode, res.stdout.strip(), res.stderr.strip()
    except Exception as e:
        return -1, "", str(e)


def fetch_system_state() -> dict:
    """Query current status and audio sources from cleanmic CLI."""
    bin_path = get_cleanmic_binary()
    if not bin_path:
        return {"error": "not_installed", "message": "cleanmic CLI executable not found"}

    code, out, err = run_cli_sync(["status"])
    if code != 0:
        return {"error": "status_failed", "message": err or out or "Failed to query status"}

    try:
        data = json.loads(out)
    except Exception as e:
        return {"error": "json_error", "message": f"Invalid JSON from status: {e}"}

    # Query sources
    code_s, out_s, _ = run_cli_sync(["sources", "--detailed"])
    if code_s != 0:
        code_s, out_s, _ = run_cli_sync(["sources"])

    sources = []
    if code_s == 0 and out_s:
        try:
            raw = json.loads(out_s)
            for item in raw:
                if isinstance(item, dict):
                    name = item.get("name", "")
                    desc = item.get("description", name)
                    if name:
                        sources.append({"name": name, "description": desc})
                elif isinstance(item, str) and item:
                    sources.append({"name": item, "description": item})
        except Exception:
            pass

    data["sources_list"] = sources

    # Check enabled state for 'Start with login'
    if "enabled" in data:
        data["is_enabled"] = bool(data["enabled"])
    elif "service_enabled" in data:
        data["is_enabled"] = bool(data["service_enabled"])
    else:
        try:
            r = subprocess.run(
                ["systemctl", "--user", "is-enabled", "clean-mic.service"],
                capture_output=True,
                text=True,
                timeout=2.0
            )
            data["is_enabled"] = (r.stdout.strip() == "enabled")
        except Exception:
            data["is_enabled"] = False

    return data


class CliWorker:
    """Background queue worker to avoid blocking the GTK UI thread."""
    def __init__(self):
        self._queue = queue.Queue()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while True:
            task = self._queue.get()
            if task is None:
                break
            func, args, callback = task
            try:
                result = func(*args)
            except Exception as e:
                result = {"error": "exception", "message": str(e)}
            if callback:
                GLib.idle_add(callback, result)
            self._queue.task_done()

    def submit(self, func, args=(), callback=None):
        self._queue.put((func, args, callback))

    def stop(self):
        self._queue.put(None)


class CleanMicWindow(Adw.ApplicationWindow):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.set_title("Clean Mic")
        self.set_default_size(520, 680)

        self._worker = CliWorker()
        self._updating_ui = False
        self._is_active = False
        self._is_enabled = False
        self._is_default = False
        self._default_error = None  # (message, monotonic expiry) after a failed `default on|off`
        self._current_source = ""
        self._sources_list = []
        self._current_settings = {}
        self._pending_slider_changes = {}
        self._inflight_slider_keys = set()
        self._pending_source_change = None
        self._apply_in_progress = False
        self._apply_pending_rerun = False
        self._debounce_timer_id = None
        self._refresh_timer_id = None
        self._query_in_progress = False

        self._build_ui()

        # Connect destroy/close to clean up timers and worker
        self.connect("close-request", self._on_close_request)

        # Start periodic status refresh (every 2 seconds)
        self._refresh_timer_id = GLib.timeout_add_seconds(2, self._on_poll_timer)

        # Initial fetch (do NOT force-start the service)
        self._trigger_refresh()

    def _build_ui(self):
        toolbar_view = Adw.ToolbarView()

        # Header Bar
        header = Adw.HeaderBar()
        btn_refresh = Gtk.Button(icon_name="view-refresh-symbolic")
        btn_refresh.set_tooltip_text("Refresh status")
        btn_refresh.connect("clicked", lambda _: self._trigger_refresh())
        header.pack_end(btn_refresh)
        toolbar_view.add_top_bar(header)

        # Alert banner for errors or missing CLI
        self._banner = Adw.Banner()
        toolbar_view.add_top_bar(self._banner)

        # Main scrollable preferences page
        scrolled = Gtk.ScrolledWindow()
        scrolled.set_vexpand(True)
        page = Adw.PreferencesPage()
        scrolled.set_child(page)
        toolbar_view.set_content(scrolled)
        self.set_content(toolbar_view)

        # Group 1: General & Service Status
        group_general = Adw.PreferencesGroup(title="Service")
        page.add(group_general)

        self._switch_noise_suppression = Adw.SwitchRow(
            title="Noise suppression",
            subtitle="Process microphone input through RNNoise filter-chain"
        )
        self._switch_noise_suppression.connect("notify::active", self._on_noise_suppression_toggled)
        group_general.add(self._switch_noise_suppression)

        self._switch_default_mic = Adw.SwitchRow(
            title="Use Clean Mic as default microphone",
            subtitle="Set clean_mic as system default input in PipeWire/PulseAudio"
        )
        self._switch_default_mic.connect("notify::active", self._on_default_mic_toggled)
        group_general.add(self._switch_default_mic)

        self._status_row = Adw.ActionRow(
            title="Status",
            subtitle="Checking status..."
        )
        self._status_icon = Gtk.Image.new_from_icon_name("media-playback-stop-symbolic")
        self._status_row.add_suffix(self._status_icon)
        group_general.add(self._status_row)

        # Group 2: Audio Device
        group_device = Adw.PreferencesGroup(title="Input Device")
        page.add(group_device)

        self._combo_mic = Adw.ComboRow(
            title="Microphone",
            subtitle="Real microphone input to clean"
        )
        empty_list = Gtk.StringList.new(["Detecting microphones..."])
        self._combo_mic.set_model(empty_list)
        self._combo_mic.connect("notify::selected", self._on_mic_selected)
        group_device.add(self._combo_mic)

        # Group 3: Filter Tuning
        group_filter = Adw.PreferencesGroup(
            title="Filter Settings",
            description="Fine-tune noise suppression and voice gating"
        )
        page.add(group_filter)

        # Suppression strength (0.0 to 1.0)
        self._row_suppression, self._scale_suppression, self._lbl_suppression, self._adj_suppression = (
            self._create_slider_row(
                title="Suppression",
                subtitle="Filter intensity (wet/dry mix)",
                lower=0.0,
                upper=1.0,
                step=0.01,
                page_step=0.1,
                digits=2,
                format_fn=lambda v: f"{int(round(v * 100))}%",
                on_change=lambda v: self._on_slider_changed("suppression", v)
            )
        )
        group_filter.add(self._row_suppression)

        # Voice sensitivity / VAD threshold (0.0 to 1.0)
        self._row_vad, self._scale_vad, self._lbl_vad, self._adj_vad = (
            self._create_slider_row(
                title="Voice sensitivity",
                subtitle="VAD threshold (gate opens when voice probability ≥ threshold)",
                lower=0.0,
                upper=1.0,
                step=0.01,
                page_step=0.1,
                digits=2,
                format_fn=lambda v: f"{v:.2f}",
                on_change=lambda v: self._on_slider_changed("vad_threshold", v)
            )
        )
        group_filter.add(self._row_vad)

        # Release ms (20 to 1000 ms)
        self._row_release, self._scale_release, self._lbl_release, self._adj_release = (
            self._create_slider_row(
                title="Gate release",
                subtitle="Hold time after voice stops before closing the gate",
                lower=20.0,
                upper=1000.0,
                step=10.0,
                page_step=50.0,
                digits=0,
                format_fn=lambda v: f"{int(round(v))} ms",
                on_change=lambda v: self._on_slider_changed("release_ms", v)
            )
        )
        group_filter.add(self._row_release)

        # Floor dB (-80 to 0 dB)
        self._row_floor, self._scale_floor, self._lbl_floor, self._adj_floor = (
            self._create_slider_row(
                title="Gate floor",
                subtitle="Background level when gate is closed (-80 dB = true silence, recommended for Meet/AGC)",
                lower=-80.0,
                upper=0.0,
                step=1.0,
                page_step=5.0,
                digits=0,
                format_fn=lambda v: "-80 dB (silence)" if int(round(v)) <= -80 else f"{int(round(v))} dB",
                on_change=lambda v: self._on_slider_changed("floor_db", v)
            )
        )
        group_filter.add(self._row_floor)

        # Group 4: Startup Options
        group_options = Adw.PreferencesGroup(title="Options")
        page.add(group_options)

        self._switch_login = Adw.SwitchRow(
            title="Start with login",
            subtitle="Automatically start Clean Mic user service on desktop login"
        )
        self._switch_login.connect("notify::active", self._on_login_toggled)
        group_options.add(self._switch_login)

    def _create_slider_row(self, title, subtitle, lower, upper, step, page_step, digits, format_fn, on_change):
        row = Adw.ActionRow(title=title, subtitle=subtitle)
        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)

        adj = Gtk.Adjustment(value=lower, lower=lower, upper=upper, step_increment=step, page_increment=page_step)
        scale = Gtk.Scale(orientation=Gtk.Orientation.HORIZONTAL, adjustment=adj)
        scale.set_digits(digits)
        scale.set_size_request(180, -1)
        scale.set_draw_value(False)
        scale.set_hexpand(True)

        val_label = Gtk.Label(label=format_fn(lower))
        val_label.set_width_chars(8)
        val_label.set_xalign(1.0)
        val_label.add_css_class("numeric")

        box.append(scale)
        box.append(val_label)
        row.add_suffix(box)

        def on_value_changed(s):
            val = s.get_value()
            val_label.set_label(format_fn(val))
            on_change(val)

        scale.connect("value-changed", on_value_changed)
        return row, scale, val_label, adj

    def _on_close_request(self, _):
        if self._refresh_timer_id:
            GLib.source_remove(self._refresh_timer_id)
            self._refresh_timer_id = None
        if self._debounce_timer_id:
            GLib.source_remove(self._debounce_timer_id)
            self._debounce_timer_id = None
        self._worker.stop()
        return False

    def _on_poll_timer(self):
        if not self._query_in_progress:
            self._trigger_refresh()
        return True

    def _trigger_refresh(self):
        self._query_in_progress = True

        def task():
            return fetch_system_state()

        def cb(state):
            self._query_in_progress = False
            self._update_ui_state(state)

        self._worker.submit(task, callback=cb)

    def _on_noise_suppression_toggled(self, row, _):
        if self._updating_ui:
            return

        target_active = row.get_active()
        if target_active == self._is_active:
            return

        def task():
            cmd = "start" if target_active else "stop"
            run_cli_sync([cmd])
            return fetch_system_state()

        self._worker.submit(task, callback=self._update_ui_state)

    def _on_default_mic_toggled(self, row, _):
        if self._updating_ui:
            return

        target_default = row.get_active()
        if target_default == self._is_default:
            return

        def task():
            cmd = "on" if target_default else "off"
            rc, out, err = run_cli_sync(["default", cmd])
            state = fetch_system_state()
            if isinstance(state, dict):
                if rc != 0:
                    detail = " ".join((err or out or f"exit code {rc}").split())
                    state["_default_error"] = f"Could not turn default microphone {cmd}: {detail}"
                else:
                    state["_default_error"] = None
            return state

        self._worker.submit(task, callback=self._update_ui_state)

    def _on_login_toggled(self, row, _):
        if self._updating_ui:
            return

        target_enabled = row.get_active()
        if target_enabled == self._is_enabled:
            return

        def task():
            cmd = "enable" if target_enabled else "disable"
            run_cli_sync([cmd])
            return fetch_system_state()

        self._worker.submit(task, callback=self._update_ui_state)

    def _on_mic_selected(self, combo, _):
        if self._updating_ui:
            return

        idx = combo.get_selected()
        if 0 <= idx < len(self._sources_list):
            new_source = self._sources_list[idx]["name"]
            if new_source and new_source != self._current_source:
                self._current_source = new_source
                self._queue_apply(source=new_source)

    def _on_slider_changed(self, key: str, value: float):
        if self._updating_ui:
            return

        # Do not trigger writes if value didn't actually change
        current_val = self._current_settings.get(key)
        if current_val is not None:
            try:
                c_val = float(current_val)
                if key in ("suppression", "vad_threshold") and abs(value - c_val) < 0.005:
                    if key in self._pending_slider_changes:
                        del self._pending_slider_changes[key]
                    return
                elif key == "release_ms" and abs(round(value) - round(c_val)) < 1:
                    if key in self._pending_slider_changes:
                        del self._pending_slider_changes[key]
                    return
                elif key == "floor_db" and abs(round(value) - round(c_val)) < 0.5:
                    if key in self._pending_slider_changes:
                        del self._pending_slider_changes[key]
                    return
            except (ValueError, TypeError):
                pass

        # Store pending change
        self._pending_slider_changes[key] = value

        # Debounce for ~500 ms before set + apply
        if self._debounce_timer_id:
            GLib.source_remove(self._debounce_timer_id)
        self._debounce_timer_id = GLib.timeout_add(500, self._on_slider_debounce_timeout)

    def _on_slider_debounce_timeout(self):
        self._debounce_timer_id = None
        if not self._pending_slider_changes:
            return False
        self._queue_apply()
        return False

    def _queue_apply(self, source: str | None = None):
        if source:
            self._pending_source_change = source

        if self._apply_in_progress:
            self._apply_pending_rerun = True
            return

        if not self._pending_slider_changes and not self._pending_source_change:
            return

        self._apply_in_progress = True
        changes = dict(self._pending_slider_changes)
        self._inflight_slider_keys = set(changes.keys())
        pending_src = self._pending_source_change
        self._pending_source_change = None
        is_active = self._is_active

        def task():
            if pending_src:
                run_cli_sync(["set", "source", pending_src])
            for k, v in changes.items():
                if k == "suppression":
                    val_str = f"{v:.4f}"
                elif k == "vad_threshold":
                    val_str = f"{v:.4f}"
                elif k == "release_ms":
                    val_str = f"{v:.1f}"
                elif k == "floor_db":
                    val_str = f"{v:.1f}"
                else:
                    val_str = str(v)
                run_cli_sync(["set", k, val_str])

            apply_cmd = ["apply"] if is_active else ["apply", "--no-restart"]
            code, _, _ = run_cli_sync(apply_cmd)
            if code != 0 and "--no-restart" in apply_cmd:
                run_cli_sync(["apply"])
            return fetch_system_state()

        def cb(state):
            for k in changes.keys():
                if k in self._pending_slider_changes and self._pending_slider_changes[k] == changes[k]:
                    del self._pending_slider_changes[k]
            self._inflight_slider_keys.clear()
            self._apply_in_progress = False

            self._update_ui_state(state)

            if self._apply_pending_rerun or self._pending_slider_changes or self._pending_source_change:
                self._apply_pending_rerun = False
                self._queue_apply()

        self._worker.submit(task, callback=cb)

    def _update_ui_state(self, state: dict):
        if not isinstance(state, dict):
            return

        # Handle CLI not found
        if state.get("error") == "not_installed":
            self._banner.set_title("Clean Mic CLI ('cleanmic') was not found. Please run ./install.sh.")
            self._banner.set_revealed(True)
            self._status_row.set_subtitle("cleanmic CLI not found")
            self._status_icon.set_from_icon_name("dialog-error-symbolic")
            self._switch_noise_suppression.set_sensitive(False)
            self._switch_default_mic.set_sensitive(False)
            self._combo_mic.set_sensitive(False)
            self._row_suppression.set_sensitive(False)
            self._row_vad.set_sensitive(False)
            self._row_release.set_sensitive(False)
            self._row_floor.set_sensitive(False)
            self._switch_login.set_sensitive(False)
            return

        # CLI is available; ensure controls are sensitive
        self._switch_noise_suppression.set_sensitive(True)
        self._switch_default_mic.set_sensitive(True)
        self._combo_mic.set_sensitive(True)
        self._row_suppression.set_sensitive(True)
        self._row_vad.set_sensitive(True)
        self._row_release.set_sensitive(True)
        self._row_floor.set_sensitive(True)
        self._switch_login.set_sensitive(True)

        service_str = state.get("service", "inactive")
        is_active = bool(state.get("service_active", False) or state.get("active", False))
        node_present = bool(state.get("node_present", False) or state.get("clean_mic_present", False))
        is_default = bool(state.get("is_default", False) or state.get("default", False))
        is_enabled = bool(state.get("is_enabled", False))

        self._is_active = is_active
        self._is_default = is_default
        self._is_enabled = is_enabled

        # Remember a `default on|off` failure so the banner survives the next polls
        if "_default_error" in state:
            msg = state["_default_error"]
            self._default_error = (msg, time.monotonic() + 10.0) if msg else None
        if self._default_error and time.monotonic() >= self._default_error[1]:
            self._default_error = None

        # Handle service error banner
        if service_str == "failed":
            self._banner.set_title("clean-mic.service failed. Check 'journalctl --user -u clean-mic.service'.")
            self._banner.set_revealed(True)
            self._status_row.set_subtitle("Failed — Service error")
            self._status_icon.set_from_icon_name("dialog-error-symbolic")
        elif state.get("error"):
            self._banner.set_title(f"Status check failed: {state.get('message', 'Unknown error')}")
            self._banner.set_revealed(True)
            self._status_row.set_subtitle("Error querying status")
            self._status_icon.set_from_icon_name("dialog-error-symbolic")
        else:
            if self._default_error:
                self._banner.set_title(self._default_error[0])
                self._banner.set_revealed(True)
            else:
                self._banner.set_revealed(False)
            if is_active and node_present:
                self._status_row.set_subtitle("Active — Clean Mic is filtering audio")
                self._status_icon.set_from_icon_name("emblem-ok-symbolic")
            elif is_active and not node_present:
                self._status_row.set_subtitle("Starting — Initializing audio filter...")
                self._status_icon.set_from_icon_name("content-loading-symbolic")
            else:
                self._status_row.set_subtitle("Inactive (stopped)")
                self._status_icon.set_from_icon_name("media-playback-stop-symbolic")

        self._updating_ui = True
        try:
            # Update Noise Suppression Switch
            if self._switch_noise_suppression.get_active() != is_active:
                self._switch_noise_suppression.set_active(is_active)

            # Update Default Mic Switch
            if self._switch_default_mic.get_active() != is_default:
                self._switch_default_mic.set_active(is_default)

            # Update Login Switch
            if self._switch_login.get_active() != is_enabled:
                self._switch_login.set_active(is_enabled)

            # Update Sources Dropdown
            raw_sources = state.get("sources_list", [])
            current_src = state.get("source", "") or state.get("settings", {}).get("source", "")
            self._current_source = current_src

            new_sources = list(raw_sources) if raw_sources else ([{"name": current_src, "description": current_src}] if current_src else [])

            # Ensure current source is present
            found_idx = -1
            for i, src in enumerate(new_sources):
                if src["name"] == current_src:
                    found_idx = i
                    break
            if current_src and found_idx == -1:
                new_sources.insert(0, {"name": current_src, "description": current_src})
                found_idx = 0

            # Compare names with existing sources list
            new_names = [s["name"] for s in new_sources]
            old_names = [s["name"] for s in self._sources_list]
            if new_names != old_names:
                self._sources_list = new_sources
                display_strings = []
                for src in self._sources_list:
                    name = src["name"]
                    desc = src.get("description", name)
                    display_strings.append(desc if desc != name else name)
                if not display_strings:
                    display_strings = ["No microphones found"]
                    self._combo_mic.set_sensitive(False)
                else:
                    self._combo_mic.set_sensitive(True)
                str_list = Gtk.StringList.new(display_strings)
                self._combo_mic.set_model(str_list)

            if found_idx >= 0 and self._combo_mic.get_selected() != found_idx:
                self._combo_mic.set_selected(found_idx)

            # Update Sliders if not actively pending or in-flight
            settings = state.get("settings", {})
            self._current_settings = dict(settings)

            active_slider_keys = set(self._pending_slider_changes.keys()) | self._inflight_slider_keys

            if "suppression" not in active_slider_keys and "suppression" in settings:
                try:
                    val = float(settings["suppression"])
                    self._scale_suppression.set_value(val)
                    self._lbl_suppression.set_label(f"{int(round(val * 100))}%")
                except (ValueError, TypeError):
                    pass

            if "vad_threshold" not in active_slider_keys and "vad_threshold" in settings:
                try:
                    val = float(settings["vad_threshold"])
                    self._scale_vad.set_value(val)
                    self._lbl_vad.set_label(f"{val:.2f}")
                except (ValueError, TypeError):
                    pass

            if "release_ms" not in active_slider_keys and "release_ms" in settings:
                try:
                    val = float(settings["release_ms"])
                    self._scale_release.set_value(val)
                    self._lbl_release.set_label(f"{int(round(val))} ms")
                except (ValueError, TypeError):
                    pass

            if "floor_db" not in active_slider_keys and "floor_db" in settings:
                try:
                    val = float(settings["floor_db"])
                    self._scale_floor.set_value(val)
                    self._lbl_floor.set_label("-80 dB (silence)" if int(round(val)) <= -80 else f"{int(round(val))} dB")
                except (ValueError, TypeError):
                    pass

        finally:
            self._updating_ui = False


class CleanMicApplication(Adw.Application):
    def __init__(self):
        super().__init__(
            application_id="io.github.cleanmic.CleanMic",
            flags=Gio.ApplicationFlags.DEFAULT_FLAGS
        )

    def do_activate(self):
        win = self.props.active_window
        if not win:
            win = CleanMicWindow(application=self)
        win.present()


def main():
    app = CleanMicApplication()
    return app.run(sys.argv)


if __name__ == "__main__":
    sys.exit(main())
