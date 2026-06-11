# MQTT case map — every kitty case → NetSDK API → safety + status

> **Generated from `src/ap2p/legacy_cases.c` v2.0.x  +  Augentix `印度
> Adiance-Augentix products group-1.xlsx` sheet `Update` (the corrected
> endpoint set)  +  `Camera_API_Testing_Checklist (2).docx`.**

| | |
|---|---|
| Total cases | **81** (0–78, 80, 81) |
| READ-only GETs | 31 |
| Mutating PUTs | 27 |
| Destructive | 1 (case 67 — SD card format) |
| v2 retained-config no-op | 1 (case 81) |
| Empty / placeholder | ~21 |

Topic conventions:
- Command IN  → `torque/rx/<SN>/<N>`
- Response OUT → `torque/tx/<SN>/<N>`
- Global broadcast → `torque/rx/all/#`

Safety categories:
- `RO`  — read-only, safe to QC any time
- `MUT` — mutates camera state, needs a known-good test payload
- `DEST` — destructive (factory format / reboot etc.) — never automate
- `SYS` — system-level (boot ping, init pulse, etc.)

| # | Method | Endpoint (legacy_cases.c) | Augentix Update sheet says | Safety | Notes |
|--:|:--|:--|:--|:--|:--|
| 0 | (publish only) | — | — | SYS | 30 s keepalive — publishes temperature + SD stats |
| 1 | (publish only) | reads `P2Pambicam_min.ini` | — | RO | Returns INI file contents |
| 2 | GET | `/netsdk/video/encode/channel/101` | same | RO | Main-stream encoder config |
| 3 | (publish only) | — | — | SYS | Empty stub |
| 4 | GET | `/netsdk/Network/Interface/1` | same | RO | LAN config |
| 5 | GET | `/netsdk/Network/Dns` | same | RO | DNS config |
| 6 | GET | `/netsdk/video/input/channel/1` | same | RO | Image params |
| 7 | GET | `/netsdk/System/deviceInfo/macAddress` | same | RO | MAC address |
| 8 | GET | `/netsdk/system/deviceinfo` | same | RO | Device info JSON |
| 9 | GET | `/NetSDK/System/time/localTime` | same | RO | Local time ISO-8601 |
| 10 | GET | `/NetSDK/Video/motionDetection/channel/1` | `/NetAPI/SmartDetect/Motion` ⚠️ | RO | **endpoint deprecated → use NetAPI** |
| 11 | GET | `/NetSdk/V2/Alarm` | — | RO | Alarm event log |
| 12–18 | (publish only) | — | — | SYS | Empty stubs |
| 19 | PUT | `/netsdk/video/encode/channel/101` | same | MUT | Set main-stream encoder |
| 20 | PUT | `/netsdk/video/input/channel/1` | same | MUT | Set image params |
| 21 | PUT | `/netsdk/image` | same | MUT | Image setting |
| 22 | (publish only) | — | — | SYS | Empty stub |
| 23 | PUT | `/netsdk/system/time/ntp` | same | MUT | **NTP set — should run on first init** |
| 24 | PUT | `/NetSDK/Video/motionDetection/channel/1` | `/NetAPI/SmartDetect/Motion` ⚠️ | MUT | **endpoint deprecated** |
| 25 | PUT | `/NetSdk/V2/Alarm` | — | MUT | |
| 26 | PUT | `/NetSdk/V2/Image/DWDR` | — | MUT | DWDR setting |
| 27 | PUT | `/NetSDK/Video/humanDetect/` | `/NetAPI/SmartDetect/Human` ⚠️ | MUT | **endpoint deprecated** |
| 28 | PUT | `/NetSdk/V2/AI/FaceDetect` | "Not available on this firmware" | MUT | Face detect (firmware-gated) |
| 29 | PUT | `/NetSdk/V2/AI/LineCrossDetect` | "Not available on this firmware" | MUT | Line-cross |
| 30 | PUT | `/NetSdk/V2/AI/HumanCounter` | — | MUT | Human counter |
| 31 | PUT | `/NetSDK/V2/AI/RegionDetect` | "Not available on this firmware" | MUT | Region detect |
| 32 | PUT | `/NetSDK/V2/AI/UnattendedObjDetect` | — | MUT | Unattended object |
| 33 | PUT | `/NetSDK/V2/AI/ObjRemoveDetect` | — | MUT | Object remove |
| 34 | (publish only) | `/Netsdk/Vmukti/DeviceInfo` (in helper) | — | RO | Vmukti device info |
| 35 | (no publish) | — | — | SYS | Empty stub |
| 36 | (no publish) | OTA download + `system("reboot")` | — | **DEST** | OTA upgrade — DESTRUCTIVE |
| 37 | (publish only) | calls `get_system_info()` | — | RO | Temperature + SD stats |
| 38 | GET | `/netsdk/image` | same | RO | Image read |
| 39 | GET | `/netsdk/video/encode/channel/102` | same | RO | Sub-stream encoder config |
| 40 | (publish only) | — | — | SYS | Empty stub |
| 41 | PUT | `/netsdk/system/operation/reboot` | same | **DEST** | Soft reboot |
| 42 | GET | `/netsdk/audio/encode/channel/101` | same | RO | Audio encode |
| 43 | PUT | `/netsdk/audio/encode/channel/101` | same | MUT | Set audio |
| 44 | (publish only) | — | — | SYS | Empty stub |
| 45 | PUT | `/NetSdk/V2/Protocol/CustomEventServer` | **deprecated** | MUT | Will be replaced with websocket |
| 46 | GET | `/NetSdk/V2/Protocol/CustomEventServer` | **deprecated** | RO | Same |
| 47 | PUT | `/NetSdk/V2/Protocol/CustomEventServer` | **deprecated** | MUT | Same |
| 48–51 | (no publish) | — | — | SYS | Empty stub |
| 52 | GET | `/NetSDK/PTZ/channel/1/setup` | `/NetSDK/PTZ/channel/1/setup` (item 13) | RO | PTZ config |
| 53 | (no publish) | — | — | SYS | Empty stub |
| 54 | GET | `/NetSdk/V2/Alarm` | — | RO | Duplicate of 11? |
| 55–57 | (no publish) | — | — | SYS | Empty stub |
| 58 | GET | `/netsdk/system/time/ntp` | same | RO | NTP read |
| 59 | PUT | `/NetSDK/System/time/timeZone` | same | MUT | **TZ set — should run on first init** |
| 60 | PUT | `/NetSDK/System/time/localTime` | same | MUT | Manual time set |
| 61 | GET | `/NetSDK/System/time/timeZone` | same | RO | TZ read |
| 62 | GET | `/NetAPI/SmartDetect/Human` | same ✅ | RO | Human detect read |
| 63 | PUT | `/NetAPI/SmartDetect/Human` | same ✅ | MUT | Human detect write |
| 64 | GET | `/NetAPI/SmartDetect/Motion` | same ✅ | RO | Motion detect read |
| 65 | PUT | `/NetAPI/SmartDetect/Motion` | same ✅ | MUT | Motion detect write |
| 66 | GET | `/NetAPI/SmartDetect/Tamper` | same ✅ | RO | Tamper read |
| 67 | PUT | `/NetSDK/SDCard/format` | — | **DEST** | SD card format |
| 68 | GET | `/NetAPI/User/List` | — | RO | User list |
| 69 | GET | `/NetSDK/SDCard/status2` | same | RO | SD status |
| 70–72 | (mixed) | `/NetAPI/SmartDetect/Tamper` PUT in 72 | same ✅ | MUT | |
| 73 | GET | `/NetAPI/System` | — | RO | Generic system |
| 74 | PUT | `/NetAPI/System` | — | MUT | |
| 75 | GET | `/NetSdk/Rtmp` | `/NetAPI/Protocol/RTMPClinet` ⚠️ | RO | **endpoint deprecated** |
| 76 | PUT | `/NetSdk/Rtmp` | `/NetAPI/Protocol/RTMPClinet` ⚠️ | MUT | **endpoint deprecated** |
| 77 | GET | `/NetAPI/Protocol/RTSPServer` | same ✅ | RO | |
| 78 | PUT | `/NetAPI/Protocol/RTSPServer` | same ✅ | MUT | |
| 80 | (no publish) | — | — | SYS | Reserved / unused |
| 81 | (handled by mqtt_thread.c, not legacy_cases) | retained config payload | — | SYS | v2.0 brand-neutral payload — applied to in-memory state |

## Cases that exist in Augentix Update sheet but are **MISSING from legacy_cases.c**

These need NEW cases added (next free numbers 79, 82+):

| Augentix Update item | Endpoint | Body example | Suggested case # |
|---|---|---|--:|
| 14 — Siren on/off | PUT `/NetAPI/Alarm/AlarmSound` | `{"Enabled":false,"Mode":"Default","DurSec":...}` | 82 |
| 15 — White light on/off | PUT `/NetAPI/Alarm/AlarmWhiteLight` | `{"Enabled":false,"Mode":"Solid","DurSec":1}` | 83 |
| 16 — Open/close telnet | PUT `/NetAPI/R.SwitchTelnet` | `{"Enable":true}` | 84 |
| 17 — HTTP/HTTPS enable | PUT `/NetAPI/Protocol/N1Server` | `{"Enable":true,"EnableTLS":false,"SearchNetPort":8002}` | 85 |
| 22 — Record encrypt | GET/PUT `/NetAPI/Product/Capability` | — | 86 |

## `run_startup_cases()` — current first-init sequence

In `legacy_cases.c:2183`:
```c
void run_startup_cases(void) {
    get_ini_files();
    cameraname_Change();       // PUT /netsdk/video/encode/channel/101 with channelName=ARCIS AI
    handle_case_6();           // GET /netsdk/video/input/channel/1
    sleep(5);
    handle_case_38();          // GET /netsdk/image
    sleep(5);
    handle_case_39();          // GET /netsdk/video/encode/channel/102
    sleep(5);
    handle_case_2();           // GET /netsdk/video/encode/channel/101
    sleep(5);
}
```

**Notable omissions (bugs):**
1. `handle_case_23()` (NTP set) is defined but **never called from startup** — that's why NTP server is blank after boot.
2. No timezone setter at all (case 59 PUT) — that's why we keep landing on `GMT+08:00` (Augentix OEM default).
3. `cameraname_Change()` always forces `"channelName":"ARCIS AI"` — fine for dev but should be per-device.
4. The 5-second sleeps add 20 s to boot time for no good reason — most NetSDK GETs return in <100 ms.

## In `mqtt_thread.c` — is the legacy switch even reached for these cases?

Yes:
```c
// mqtt_thread.c:101–113
switch (case_num) {
    case 81: { ... apply_ap2p_conf_payload(...) ... }
    default:
        legacy_message_arrived(g_client, topicName, topicLen, message);
        break;
}
```

But `run_startup_cases()` is **NOT** invoked from `mqtt_thread.c`.  Only `ap2p_legacy_init()` is called after connect — that hands over the client pointer + creds but doesn't fire the startup pulse.

That's the root of the user's "MQTT pipeline is broken" — **the auto-init sequence never runs in v2.x**.

## Self-QC harness

`tools/mqtt-qc.py` (next commit) drives every RO case, validates response, generates HTML report.  MUT cases tested with known-safe payloads.  DEST cases skipped automatically.
