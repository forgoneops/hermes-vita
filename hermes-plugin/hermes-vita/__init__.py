"""hermes-vita: forwards Hermes lifecycle events to the Hermes Vita bridge on this PC (loopback, with a key).

Nothing leaves the machine: the bridge listens on 127.0.0.1 for this plugin and on the LAN only for a paired Vita.
Turns that come from the bridge's own API-server chat are ignored by the bridge, so nothing is shown twice.
Standard library only. Every call is fire-and-forget on a short timeout: a missing bridge never slows or breaks Hermes.
"""
from __future__ import annotations

import json
import os
import threading
import urllib.request

_URL = os.environ.get("HERMES_VITA_URL", "http://127.0.0.1:4410").rstrip("/") + "/_plugin/event"
_KEY_FILE = os.path.join(os.environ.get("HERMES_VITA_HOME", os.path.join(os.path.expanduser("~"), ".hermes-vita")), "plugin-key")


def _key() -> str:
    try:
        with open(_KEY_FILE, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return os.environ.get("HERMES_VITA_KEY", "")


def _send(event: str, **data) -> None:
    key = _key()
    if not key:
        return
    body = json.dumps({"event": event, **data}).encode("utf-8")

    def go() -> None:
        try:
            req = urllib.request.Request(_URL, data=body, headers={"content-type": "application/json", "x-hermes-vita-key": key})
            urllib.request.urlopen(req, timeout=1.5).read()
        except Exception:
            pass

    threading.Thread(target=go, daemon=True).start()


def _turn_start(**kw):
    _send("turn_start", platform=kw.get("platform", ""), text=str(kw.get("user_message", ""))[:600])


def _turn_end(**kw):
    _send("turn_end", platform=kw.get("platform", ""), text=str(kw.get("assistant_response", ""))[:4000])


def _tool_start(**kw):
    _send("tool_start", tool=str(kw.get("tool_name", ""))[:40])


def _tool_end(**kw):
    _send("tool_end")


def _error(**kw):
    _send("error")


def _stopped(**kw):
    _send("stop")


def register(ctx):
    ctx.register_hook("pre_llm_call", _turn_start)
    ctx.register_hook("post_llm_call", _turn_end)
    ctx.register_hook("pre_tool_call", _tool_start)
    ctx.register_hook("post_tool_call", _tool_end)
    ctx.register_hook("api_request_error", _error)
    ctx.register_hook("agent_loop_stopped", _stopped)
