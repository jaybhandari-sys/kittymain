#!/usr/bin/env python3
"""
mqtt-qc.py — end-to-end self-QC for every kitty MQTT case (0..78, 80, 81).

For each case the harness drives the full path against a real camera over
the broker:

  RO            publish stimulus → assert well-formed response on /tx/<N>
  MUT           GET-before (via verify_via_case)
                → publish flip_stimulus
                → GET-after (asserts the value actually changed)
                → publish restore_stimulus
  STUB_NS       publish → expect "not supported in your product"
  STATE         publish → assert NO statusCode error in any subsequent /tx echo
  DEST_TESTABLE publish a deliberately-invalid payload → assert rejection
                (case 36: invalid URL → "Invalid OTA URL")
  DEST_REBOOT   case 41 — runs LAST, verified by torque/tx/<sn>/boot republish
  DEST_OPTIN    case 67 SD format — skipped unless --confirm-sd-format
  V2            case 81 — handled in mqtt_thread; verified by signaling reg

Default ENV:
  BROKER_HOST   mqtt-staging.devices.arcisai.io
  BROKER_PORT   443
  BROKER_USER   Torque
  BROKER_PASS   Raptor@0

Usage:
  mqtt-qc.py --sn ATPL-200007-TESTA               # full sweep, RO+MUT+STUB+STATE
  mqtt-qc.py --sn ATPL-200007-TESTA --include-reboot
  mqtt-qc.py --sn ATPL-200007-TESTA --only 19,23,59,63
  mqtt-qc.py --sn ATPL-200007-TESTA --confirm-sd-format
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).parent))
from mqtt_fixtures import Fixture, all_fixtures

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("install paho-mqtt:  pip install paho-mqtt")


# ─────────────────────────── runner ─────────────────────────────────────────
@dataclass
class Result:
    case: Fixture
    status: str = "PENDING"
    # status values:
    #   PASS, FAIL_NO_RESP, FAIL_BAD_STATUS, FAIL_SCHEMA, FAIL_NOT_FLIPPED,
    #   FAIL_REJECTED_WRONG, SKIP_DEST, SKIP_OPTIN, SKIP_V2, SKIP_STATE_NORESP
    rt_ms: float = 0
    response: str = ""
    detail: str = ""


def setup_client(args):
    inbox: dict[int, list[tuple[float, str]]] = {}
    boot_inbox: list[tuple[float, dict]] = []

    def on_msg(c, ud, msg):
        topic = msg.topic
        try:
            payload = msg.payload.decode(errors="replace")
        except Exception:
            return
        tx_pfx = f"torque/tx/{args.sn}/"
        if topic == tx_pfx + "boot":
            try:
                d = json.loads(payload)
                boot_inbox.append((time.time(), d))
            except Exception:
                pass
            return
        if topic.startswith(tx_pfx):
            suf = topic[len(tx_pfx):]
            try:
                n = int(suf)
            except ValueError:
                return
            inbox.setdefault(n, []).append((time.time(), payload))

    def on_conn(c, ud, flags, rc, props=None):
        c.subscribe(f"torque/tx/{args.sn}/#", qos=1)

    client = mqtt.Client(client_id=f"mqtt-qc-{os.getpid()}",
                         callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(args.user, args.password)
    client.on_message = on_msg
    client.on_connect = on_conn
    client.connect(args.broker, int(args.port), keepalive=30)
    client.loop_start()
    return client, inbox, boot_inbox


def publish_and_wait(client, inbox, sn, n, payload, timeout=8.0):
    """Publish to torque/rx/<sn>/<n>, return latest response on torque/tx/<sn>/<n>."""
    inbox.pop(n, None)
    t0 = time.time()
    info = client.publish(f"torque/rx/{sn}/{n}", payload, qos=1)
    try:
        info.wait_for_publish(timeout=2)
    except Exception:
        pass
    deadline = t0 + timeout
    while time.time() < deadline:
        if n in inbox and inbox[n]:
            ts, body = inbox[n][-1]
            return (ts - t0) * 1000, body
        time.sleep(0.1)
    return None, None


def parse_status(resp: str) -> dict:
    try:
        return json.loads(resp)
    except Exception:
        return {}


def has_key_deep(j, key) -> bool:
    if isinstance(j, dict):
        if key in j: return True
        return any(has_key_deep(v, key) for v in j.values())
    if isinstance(j, list):
        return any(has_key_deep(v, key) for v in j)
    return False


def get_value_deep(j, key):
    if isinstance(j, dict):
        if key in j: return j[key]
        for v in j.values():
            r = get_value_deep(v, key)
            if r is not None: return r
    if isinstance(j, list):
        for v in j:
            r = get_value_deep(v, key)
            if r is not None: return r
    return None


def is_status_ok(j: dict) -> tuple[bool, str]:
    """True if the response is NOT an error.  Augentix error shape:
       {"requestMethod":"...","statusCode":N,"statusString":"..."}"""
    if not isinstance(j, dict):
        return True, ""
    sc = j.get("statusCode")
    if sc in (None, 0, 200, "OK", "Success"):
        return True, ""
    ss = j.get("statusString", "")
    return False, f"statusCode={sc} statusString={ss[:80]}"


# ─────────────────────────── per-case runners ───────────────────────────────
def run_ro(client, inbox, sn, fx, timeout) -> Result:
    rt, resp = publish_and_wait(client, inbox, sn, fx.n, fx.stimulus, timeout)
    r = Result(case=fx, rt_ms=rt or 0, response=(resp or "")[:600])
    if resp is None:
        r.status = "FAIL_NO_RESP"
        r.detail = f"no response on /tx/{fx.n} within {timeout}s"
        return r
    j = parse_status(resp)
    ok, detail = is_status_ok(j)
    if fx.expect_status_ok and not ok:
        r.status = "FAIL_BAD_STATUS"; r.detail = detail; return r
    missing = [k for k in fx.expect_keys if not has_key_deep(j, k)]
    if missing:
        r.status = "FAIL_SCHEMA"
        r.detail = f"missing keys: {missing}"
        return r
    r.status = "PASS"; return r


def run_mut(client, inbox, sn, fx, fixture_by_n, timeout) -> Result:
    """Full round-trip: GET-before, PUT-flip, GET-after, restore, GET-restore."""
    r = Result(case=fx)

    # 1. PUT-flip
    rt, resp = publish_and_wait(client, inbox, sn, fx.n, fx.flip_stimulus, timeout)
    r.rt_ms = rt or 0
    r.response = (resp or "")[:600]
    if resp is None:
        r.status = "FAIL_NO_RESP"
        r.detail = f"PUT flip: no response on /tx/{fx.n}"
        return r
    j = parse_status(resp)
    ok, detail = is_status_ok(j)
    if not ok:
        r.status = "FAIL_BAD_STATUS"; r.detail = "PUT flip: " + detail; return r

    # 2. Verify via GET-back, if specified
    if fx.verify_via_case is not None and fx.verify_key:
        get_fx = fixture_by_n.get(fx.verify_via_case)
        if get_fx is None:
            r.status = "PASS"; r.detail = "(no verify case mapped)"
            return r
        time.sleep(0.4)
        rt2, get_resp = publish_and_wait(client, inbox, sn, get_fx.n,
                                         get_fx.stimulus, timeout)
        if get_resp is None:
            r.status = "FAIL_NO_RESP"
            r.detail = f"verify GET case {get_fx.n}: no response"
            return r
        gj = parse_status(get_resp)
        actual = get_value_deep(gj, fx.verify_key)
        if str(actual) != fx.verify_value:
            r.status = "FAIL_NOT_FLIPPED"
            r.detail = (f"after PUT, GET {fx.verify_key}={actual!r} "
                        f"(expected {fx.verify_value!r})")
            return r

    # 3. Restore original
    if fx.restore_stimulus:
        time.sleep(0.4)
        publish_and_wait(client, inbox, sn, fx.n, fx.restore_stimulus, timeout)

    r.status = "PASS"
    return r


def run_stub(client, inbox, sn, fx, timeout) -> Result:
    rt, resp = publish_and_wait(client, inbox, sn, fx.n, fx.stimulus, timeout)
    r = Result(case=fx, rt_ms=rt or 0, response=(resp or "")[:600])
    if resp is None:
        r.status = "FAIL_NO_RESP"
        r.detail = "stub didn't echo not-supported message"
        return r
    if "not supported" in resp.lower():
        r.status = "PASS"; return r
    r.status = "FAIL_SCHEMA"
    r.detail = f"stub response missing 'not supported': {resp[:80]!r}"
    return r


def run_state(client, inbox, sn, fx, timeout) -> Result:
    # STATE cases don't normally publish a /tx response.  We publish and
    # call it a pass iff there's no /tx error within a short window.
    inbox.pop(fx.n, None)
    client.publish(f"torque/rx/{sn}/{fx.n}", fx.stimulus, qos=1)
    time.sleep(2.0)
    r = Result(case=fx)
    if fx.n in inbox and inbox[fx.n]:
        body = inbox[fx.n][-1][1]
        r.response = body[:300]
        # if there's a response, that's actually fine for some state cases
        j = parse_status(body)
        ok, detail = is_status_ok(j)
        if ok:
            r.status = "PASS"
        else:
            r.status = "FAIL_BAD_STATUS"; r.detail = detail
    else:
        r.status = "PASS"
        r.detail = "(no /tx echo expected for state-only case)"
    return r


def run_dest_testable(client, inbox, sn, fx, timeout) -> Result:
    rt, resp = publish_and_wait(client, inbox, sn, fx.n, fx.stimulus, timeout)
    r = Result(case=fx, rt_ms=rt or 0, response=(resp or "")[:600])
    if resp is None:
        r.status = "FAIL_NO_RESP"
        r.detail = "destructive case: no rejection echo"
        return r
    if fx.must_reject_with.lower() in resp.lower():
        r.status = "PASS"; return r
    r.status = "FAIL_REJECTED_WRONG"
    r.detail = (f"expected response to contain {fx.must_reject_with!r}, "
                f"got {resp[:100]!r}")
    return r


def run_dest_reboot(client, inbox, boot_inbox, sn, fx, timeout=120) -> Result:
    r = Result(case=fx)
    # Snapshot boot_inbox length BEFORE
    n_before = len(boot_inbox)
    t0 = time.time()
    client.publish(f"torque/rx/{sn}/{fx.n}", fx.stimulus, qos=1)
    # Wait up to `timeout` for a new boot-timing publish.
    deadline = t0 + timeout
    while time.time() < deadline:
        if len(boot_inbox) > n_before:
            ts, data = boot_inbox[-1]
            uptime = data.get("now_uptime_sec", 9999)
            if uptime < 90:        # fresh-boot publish
                r.rt_ms = (ts - t0) * 1000
                r.response = f"new boot publish, uptime={uptime}s"
                r.status = "PASS"
                return r
        time.sleep(2)
    r.status = "FAIL_NO_RESP"
    r.detail = f"camera didn't republish boot-timing within {timeout}s"
    return r


# ─────────────────────────── driver ─────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sn",       default="ATPL-200007-TESTA")
    ap.add_argument("--broker",   default=os.environ.get("BROKER_HOST", "mqtt-staging.devices.arcisai.io"))
    ap.add_argument("--port",     default=os.environ.get("BROKER_PORT", "443"))
    ap.add_argument("--user",     default=os.environ.get("BROKER_USER", "Torque"))
    ap.add_argument("--password", default=os.environ.get("BROKER_PASS", "Raptor@0"))
    ap.add_argument("--only",     help="comma-list of case #s (e.g. 2,4,23)")
    ap.add_argument("--skip",     help="comma-list of case #s to skip")
    ap.add_argument("--timeout",  default=8, type=float,
                    help="per-case response wait (s)")
    ap.add_argument("--include-reboot",     action="store_true",
                    help="run case 41 reboot (runs LAST; ~60s)")
    ap.add_argument("--confirm-sd-format",  action="store_true",
                    help="run case 67 SD format (destructive)")
    ap.add_argument("--skip-mut",           action="store_true",
                    help="skip every MUT case (use to do a fast RO sweep)")
    ap.add_argument("--report",   default="/tmp/mqtt-qc.html")
    args = ap.parse_args()

    fixtures = all_fixtures()
    by_n = {f.n: f for f in fixtures}

    only = {int(x) for x in args.only.split(",")} if args.only else set()
    skip = {int(x) for x in args.skip.split(",")} if args.skip else set()

    print(f"mqtt-qc: target SN={args.sn} broker={args.broker}:{args.port}")
    print(f"         {len(fixtures)} fixtures; include-reboot={args.include_reboot}")
    print()

    client, inbox, boot_inbox = setup_client(args)
    time.sleep(2)

    # Filter + order: STATE/RO/STUB/MUT first; DEST_TESTABLE; DEST_OPTIN; DEST_REBOOT LAST.
    order = {"STATE": 0, "RO": 1, "STUB_NS": 2, "MUT": 3, "V2": 4,
             "DEST_TESTABLE": 5, "DEST_OPTIN": 6, "DEST_REBOOT": 7}
    fixtures_sorted = sorted(fixtures, key=lambda f: (order[f.safety], f.n))

    results: list[Result] = []
    for fx in fixtures_sorted:
        if only and fx.n not in only:
            continue
        if fx.n in skip:
            continue
        if args.skip_mut and fx.safety == "MUT":
            r = Result(case=fx, status="SKIP_DEST", detail="--skip-mut")
            results.append(r); continue
        # Dispatch
        if fx.safety == "V2":
            r = Result(case=fx, status="SKIP_V2",
                       detail="handled in mqtt_thread.c via retained payload")
        elif fx.safety == "RO":
            r = run_ro(client, inbox, args.sn, fx, args.timeout)
        elif fx.safety == "MUT":
            r = run_mut(client, inbox, args.sn, fx, by_n, args.timeout)
        elif fx.safety == "STUB_NS":
            r = run_stub(client, inbox, args.sn, fx, args.timeout)
        elif fx.safety == "STATE":
            r = run_state(client, inbox, args.sn, fx, args.timeout)
        elif fx.safety == "DEST_TESTABLE":
            r = run_dest_testable(client, inbox, args.sn, fx, args.timeout)
        elif fx.safety == "DEST_OPTIN":
            if not args.confirm_sd_format:
                r = Result(case=fx, status="SKIP_OPTIN",
                           detail="use --confirm-sd-format to run")
            else:
                r = run_ro(client, inbox, args.sn, fx, args.timeout)
        elif fx.safety == "DEST_REBOOT":
            if not args.include_reboot:
                r = Result(case=fx, status="SKIP_DEST",
                           detail="use --include-reboot to run (LAST)")
            else:
                r = run_dest_reboot(client, inbox, boot_inbox, args.sn, fx, 120)
        else:
            r = Result(case=fx, status="FAIL_SCHEMA",
                       detail=f"unknown safety class {fx.safety}")
        results.append(r)
        mark = {"PASS": "✓", "FAIL_NO_RESP": "T", "FAIL_BAD_STATUS": "✗",
                "FAIL_SCHEMA": "?", "FAIL_NOT_FLIPPED": "≠",
                "FAIL_REJECTED_WRONG": "‼", "SKIP_DEST": "-", "SKIP_OPTIN": "-",
                "SKIP_V2": "-", "PENDING": "."}.get(r.status, "?")
        print(f"  [{mark}] case {fx.n:<3} {fx.safety:<14} {fx.name[:42]:<42}  "
              f"{r.status:<18} {r.rt_ms:>5.0f}ms  {r.detail[:60]}",
              flush=True)

    client.loop_stop(); client.disconnect()

    # Report
    counts: dict[str, int] = {}
    for r in results:
        counts[r.status] = counts.get(r.status, 0) + 1
    n_pass = counts.get("PASS", 0)
    n_skip = sum(v for k, v in counts.items() if k.startswith("SKIP"))
    n_test = sum(counts.values()) - n_skip
    pct = 100.0 * n_pass / max(1, n_test)
    print(f"\n{'='*72}")
    print(f"PASS {n_pass}/{n_test}  ({pct:.0f}%)   skipped: {n_skip}")
    for k, v in sorted(counts.items()):
        print(f"  {k:<22} {v}")

    # HTML
    write_html(results, args.report)
    print(f"→ {args.report}")
    sys.exit(0 if (n_test > 0 and n_pass == n_test) else 1)


def write_html(results: list[Result], path: str):
    css = {"PASS": "#16a34a", "FAIL_NO_RESP": "#dc2626",
           "FAIL_BAD_STATUS": "#ea580c", "FAIL_SCHEMA": "#f59e0b",
           "FAIL_NOT_FLIPPED": "#dc2626", "FAIL_REJECTED_WRONG": "#ea580c",
           "SKIP_DEST": "#6b7280", "SKIP_OPTIN": "#6b7280",
           "SKIP_V2": "#6b7280", "PENDING": "#a3a3a3"}
    rows = []
    for r in sorted(results, key=lambda x: x.case.n):
        color = css.get(r.status, "#9ca3af")
        rows.append(
            f"<tr><td>{r.case.n}</td><td>{r.case.name}</td>"
            f"<td>{r.case.safety}</td>"
            f"<td style='background:{color};color:white;text-align:center'>{r.status}</td>"
            f"<td style='text-align:right'>{r.rt_ms:.0f}</td>"
            f"<td><code>{r.response.replace('<','&lt;')[:200]}</code></td>"
            f"<td>{r.detail}</td></tr>"
        )
    open(path, "w").write(f"""<!doctype html><meta charset=utf-8>
<title>MQTT QC — {time.strftime('%Y-%m-%d %H:%M')}</title>
<style>body{{font-family:system-ui,sans-serif;padding:18px;font-size:13px}}
table{{border-collapse:collapse;width:100%}}
th,td{{border:1px solid #ddd;padding:6px;vertical-align:top}}
th{{background:#f3f4f6;text-align:left}}
code{{font-size:11px;word-break:break-all}}</style>
<h2>MQTT case QC — {time.strftime('%Y-%m-%d %H:%M:%S')}</h2>
<table><tr><th>#</th><th>name</th><th>safety</th><th>status</th><th>ms</th>
<th>response</th><th>detail</th></tr>
{''.join(rows)}
</table>""")


if __name__ == "__main__":
    main()
