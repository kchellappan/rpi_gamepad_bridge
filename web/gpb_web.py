#!/usr/bin/env python3
"""Control panel for the gamepad bridge.

Deliberately a separate process from gpbridge. The bridge is a real-time hot path -- an HTTP
server inside it would add allocation, threads and network exposure to the one process whose
job is to not introduce jitter -- and a process cannot cleanly restart itself anyway.

Standard library only: no pip, no build step, nothing to install on the Pi.

Privilege model: this runs as an unprivileged user. The three things it cannot do directly
come from a narrowly scoped sudoers rule (see scripts/install_web.sh), not from running as
root.
"""
from __future__ import annotations

import base64
import hmac
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO = Path(os.environ.get("GPB_REPO", Path(__file__).resolve().parent.parent))
ENVFILE = Path(os.environ.get("GPB_ENVFILE", "/etc/gpbridge/active.env"))
PASSFILE = Path(os.environ.get("GPB_PASSFILE", "/etc/gpbridge/webpass"))
USERFILE = Path(os.environ.get("GPB_USERFILE", "/etc/gpbridge/webuser"))
DEFAULT_USER = "admin"
PORT = int(os.environ.get("GPB_PORT", "8080"))
STATIC = Path(__file__).resolve().parent / "static"
CONFIG_DIR = REPO / "config"

BRIDGE_UNIT = "gpbridge.service"
GADGET_UNIT = "gpb-gadget.service"

_env_lock = threading.Lock()


# --------------------------------------------------------------------------- shell helpers

def run(cmd: list[str], timeout: int = 15) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, f"timed out: {' '.join(cmd)}"
    except FileNotFoundError:
        return 127, f"not found: {cmd[0]}"


def systemctl(action: str, unit: str) -> tuple[int, str]:
    # Whitelisted here as well as in sudoers: defence in depth costs nothing, and it keeps
    # the set of reachable actions obvious from this file alone.
    if action not in {"start", "stop", "restart"} or unit not in {BRIDGE_UNIT, GADGET_UNIT}:
        return 1, f"refused: {action} {unit}"
    return run(["sudo", "-n", "systemctl", action, unit])


def unit_state(unit: str) -> dict:
    _, active = run(["systemctl", "is-active", unit])
    _, enabled = run(["systemctl", "is-enabled", unit])
    rc, props = run(["systemctl", "show", unit, "-p", "ActiveEnterTimestamp", "-p", "NRestarts"])
    since, restarts = "", ""
    for line in props.splitlines():
        if line.startswith("ActiveEnterTimestamp="):
            since = line.split("=", 1)[1]
        elif line.startswith("NRestarts="):
            restarts = line.split("=", 1)[1]
    return {
        "active": active.strip(),
        "enabled": enabled.strip(),
        "since": since,
        "restarts": restarts,
    }


def udc_state() -> dict:
    """Whether a USB host is actually attached, read straight from sysfs."""
    base = Path("/sys/class/udc")
    if not base.is_dir():
        return {"present": False}
    for udc in sorted(base.iterdir()):
        def read(name):
            try:
                return (udc / name).read_text().strip()
            except OSError:
                return "?"
        return {
            "present": True,
            "name": udc.name,
            "state": read("state"),
            "speed": read("current_speed"),
        }
    return {"present": False}


# --------------------------------------------------------------------------- active config

def read_env() -> dict:
    values = {"GPB_CONFIG": "", "GPB_SOURCE": "evdev"}
    try:
        for line in ENVFILE.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k.strip() in values:
                values[k.strip()] = v.strip()
    except OSError:
        pass
    return values


def write_env(config: str, source: str) -> tuple[bool, str]:
    if source not in {"evdev", "socket"}:
        return False, f"unknown source: {source}"
    available = {str(p) for p in list_configs()}
    if config not in available:
        return False, "config is not one of the known files"
    body = (
        "# Written by gpb-web. The .ini files remain the source of truth;\n"
        "# this only selects which one the service starts with.\n"
        f"GPB_CONFIG={config}\n"
        f"GPB_SOURCE={source}\n"
    )
    with _env_lock:
        try:
            tmp = ENVFILE.with_suffix(".env.tmp")
            tmp.write_text(body)
            os.replace(tmp, ENVFILE)     # atomic: never leave a half-written env file
        except OSError as e:
            return False, f"cannot write {ENVFILE}: {e}"
    return True, "saved"


def list_configs() -> list[Path]:
    if not CONFIG_DIR.is_dir():
        return []
    return sorted(p for p in CONFIG_DIR.glob("*.ini") if p.is_file())


def config_summary(path: Path) -> dict:
    """Pull the few fields worth showing next to a config's name.

    [meta] name/description are optional and purely descriptive -- nothing but the UI reads
    them. A config without them falls back to its filename, so older files keep working.
    """
    out = {"source": "", "sink": "", "device": "", "heartbeat": "",
           "title": path.stem, "description": ""}
    try:
        section = ""
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].split(";", 1)[0].strip()
            if not line:
                continue
            if line.startswith("["):
                section = line.strip("[]").strip()
                continue
            if "=" not in line:
                continue
            k, v = (x.strip() for x in line.split("=", 1))
            if section == "meta" and k == "name" and v:
                out["title"] = v
            elif section == "meta" and k == "description":
                out["description"] = v
            elif section == "bridge" and k in ("source", "sink"):
                out[k] = v
            elif section == "source.evdev" and k == "device":
                out["device"] = v
            elif section == "sink.ns_hid" and k == "heartbeat_hz":
                out["heartbeat"] = v
    except OSError:
        pass
    return out


# --------------------------------------------------------------------------- auth

def load_credentials() -> tuple[str, str | None]:
    """Read the username and password from disk on every call.

    Deliberately not cached at startup. Caching meant that editing the password file did
    nothing until someone thought to restart the service -- a silent failure, and exactly
    the kind of trap that leads to believing a password has been rotated when it has not.
    Two small file reads on a page that polls once a second is not a cost worth optimising.
    """
    user = DEFAULT_USER
    try:
        candidate = USERFILE.read_text().strip()
        if candidate:
            user = candidate
    except OSError:
        pass
    try:
        password = PASSFILE.read_text().strip() or None
    except OSError:
        password = None
    return user, password


def authorized(header: str | None) -> bool:
    expected_user, expected_password = load_credentials()

    # No password file means auth is disabled. install_web.sh always generates one; this
    # path exists for running the server by hand during development.
    if not expected_password:
        return True
    if not header or not header.startswith("Basic "):
        return False
    try:
        decoded = base64.b64decode(header[6:]).decode("utf-8", "replace")
    except Exception:
        return False
    supplied_user, _, supplied_password = decoded.partition(":")
    # Compare both in constant time, and evaluate both halves rather than short-circuiting,
    # so the response time does not reveal which field was wrong.
    user_ok = hmac.compare_digest(supplied_user, expected_user)
    password_ok = hmac.compare_digest(supplied_password, expected_password)
    return user_ok and password_ok


# --------------------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = "gpb-web"

    def log_message(self, fmt, *args):      # quieter journal
        sys.stderr.write("[web] %s\n" % (fmt % args))

    # -- helpers
    def _send(self, code, body: bytes, ctype="application/json", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=HTTPStatus.OK):
        self._send(code, json.dumps(obj).encode())

    def _deny(self):
        self._send(HTTPStatus.UNAUTHORIZED, b'{"error":"auth required"}',
                   extra={"WWW-Authenticate": 'Basic realm="gamepad bridge"'})

    def _guard(self) -> bool:
        if authorized(self.headers.get("Authorization")):
            return True
        self._deny()
        return False

    # -- routes
    def do_GET(self):
        if not self._guard():
            return
        path = self.path.split("?", 1)[0]

        if path in ("/", "/index.html"):
            return self._static("index.html", "text/html; charset=utf-8")
        if path == "/style.css":
            return self._static("style.css", "text/css")
        if path == "/app.js":
            return self._static("app.js", "application/javascript")
        if path == "/api/status":
            return self._json(self.status())
        if path == "/api/logs":
            return self._json({"lines": self.logs()})
        self._send(HTTPStatus.NOT_FOUND, b'{"error":"not found"}')

    def do_POST(self):
        if not self._guard():
            return
        path = self.path.split("?", 1)[0]
        length = int(self.headers.get("Content-Length") or 0)
        try:
            payload = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            return self._json({"ok": False, "message": "bad JSON"}, HTTPStatus.BAD_REQUEST)

        if path == "/api/service":
            action = payload.get("action", "")
            rc, out = systemctl(action, BRIDGE_UNIT)
            return self._json({"ok": rc == 0, "message": out.strip() or f"{action} ok"})

        if path == "/api/gadget":
            # Escape hatch only. Re-enumerating shows the console a controller disconnect,
            # so it is never done automatically -- a config change restarts the bridge and
            # leaves the USB device in place.
            rc, out = systemctl("restart", GADGET_UNIT)
            return self._json({"ok": rc == 0, "message": out.strip() or "gadget re-enumerated"})

        if path == "/api/select":
            ok, msg = write_env(payload.get("config", ""), payload.get("source", ""))
            if ok and payload.get("restart", True):
                rc, out = systemctl("restart", BRIDGE_UNIT)
                return self._json({"ok": rc == 0,
                                   "message": out.strip() or "saved and restarted"})
            return self._json({"ok": ok, "message": msg})

        self._send(HTTPStatus.NOT_FOUND, b'{"error":"not found"}')

    # -- data
    def _static(self, name, ctype):
        try:
            self._send(HTTPStatus.OK, (STATIC / name).read_bytes(), ctype)
        except OSError:
            self._send(HTTPStatus.NOT_FOUND, b"missing static asset", "text/plain")

    def status(self) -> dict:
        env = read_env()
        configs = []
        for p in list_configs():
            configs.append({"path": str(p), "name": p.name, **config_summary(p)})
        return {
            "bridge": unit_state(BRIDGE_UNIT),
            "gadget": unit_state(GADGET_UNIT),
            "udc": udc_state(),
            "active": {"config": env["GPB_CONFIG"], "source": env["GPB_SOURCE"]},
            "configs": configs,
        }

    def logs(self) -> list[str]:
        m = re.search(r"[?&]n=(\d+)", self.path)
        n = min(int(m.group(1)) if m else 200, 1000)
        rc, out = run(["sudo", "-n", "journalctl", "-u", BRIDGE_UNIT,
                       "-n", str(n), "--no-pager", "-o", "short-iso"])
        if rc != 0:
            return [f"(could not read the journal: {out.strip()})"]
        return out.splitlines()


def main():
    if not shutil.which("systemctl"):
        print("systemctl not found; this is meant to run on the Pi", file=sys.stderr)
    web_user, web_password = load_credentials()
    if not web_password:
        print(f"[web] no password at {PASSFILE} -- authentication DISABLED", file=sys.stderr)
    else:
        print(f"[web] authenticating as user '{web_user}'", file=sys.stderr)
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"[web] listening on 0.0.0.0:{PORT}  repo={REPO}  env={ENVFILE}", file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
