"""
MQTT case test fixtures — one entry per kitty case (0..78, 80, 81).

Imported by tools/mqtt-qc.py.  Each fixture is a `Fixture` carrying enough
information for the harness to drive the case end-to-end including, for
MUT cases, a GET-back verification step and a restore step.

Safety classes (mirror docs/MQTT_CASES.md):
  RO              — read-only; publish stimulus, validate response schema
  MUT             — mutating; round-trip GET-before → PUT-flip → GET-after → restore
  DEST_TESTABLE   — destructive but has a "safe-rejection" path we can verify
                    (e.g., case 36 OTA must reject a non-http URL without flashing)
  DEST_REBOOT     — case 41 — runs LAST in the QC sequence; verified by camera
                    coming back live (a fresh boot-timing publish on torque/tx/<sn>/boot)
  DEST_OPTIN      — case 67 SD-format; skipped unless --confirm-sd-format
  STUB_NS         — returns canned `{"Data":"not supported in your product"}`
                    — these are correct behavior, just empty endpoints
  STATE           — cases 0/1/56/57/80 — special state-management handlers, not a
                    request/response API.  Verified by side-effect checks.
  V2              — case 81 — handled in mqtt_thread.c, not legacy_cases.c
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional


@dataclass
class Fixture:
    n: int
    name: str
    safety: str                    # RO|MUT|DEST_TESTABLE|DEST_REBOOT|DEST_OPTIN|STUB_NS|STATE|V2
    stimulus: str = ""             # what we publish to torque/rx/<sn>/<N>
    expect_keys: tuple = ()        # required keys in response JSON
    expect_value: dict = field(default_factory=dict)
                                   # key→value pairs to verify
    expect_status_ok: bool = True  # require statusCode==0 or 200 if present
    # MUT-specific:
    flip_stimulus: str = ""        # the value we PUT to flip
    verify_via_case: Optional[int] = None  # case # to GET-back the value
    verify_key: str = ""           # field name in GET response to check
    verify_value: str = ""         # expected post-flip value
    restore_stimulus: str = ""     # restore original value
    # DEST_TESTABLE-specific:
    must_reject_with: str = ""     # substring expected in rejection response


# ---- READ-ONLY cases (publish stimulus → expect well-formed response) ------
RO = [
    Fixture(2,  "encoder ch101 GET (main)",          "RO", "2",
            expect_keys=("resolution", "codecType", "channelName"),
            expect_status_ok=False),                 # ch101 returns config, no statusCode field
    Fixture(4,  "Network/Interface/1 GET",           "RO", "4"),
    Fixture(5,  "Network/Dns GET",                   "RO", "5"),
    Fixture(6,  "video/input/channel/1 GET",         "RO", "6",
            expect_keys=("brightnessLevel", "contrastLevel"), expect_status_ok=False),
    Fixture(7,  "macAddress GET",                    "RO", "7",
            expect_keys=("macAddress",), expect_status_ok=False),
    Fixture(8,  "deviceInfo GET",                    "RO", "8",
            expect_keys=("deviceName", "firmwareVersion", "serialNumber"),
            expect_status_ok=False),
    Fixture(9,  "localTime GET",                     "RO", "9"),
    Fixture(10, "motionDetection ch1 GET (deprecated NetSDK)", "RO", "10"),
    Fixture(11, "V2/Alarm GET",                      "RO", "11"),
    Fixture(34, "Vmukti/DeviceInfo GET",             "RO", "34"),
    Fixture(35, "config.json read",                  "RO", "35",
            expect_keys=("mqttUrl",), expect_status_ok=False),
    Fixture(37, "system_info (temp + SD)",           "RO", "37",
            expect_keys=("temperature",), expect_status_ok=False),
    Fixture(38, "image GET",                         "RO", "38"),
    Fixture(39, "encoder ch102 GET (sub)",           "RO", "39",
            expect_keys=("resolution",), expect_status_ok=False),
    Fixture(42, "audio encode ch101 GET",            "RO", "42"),
    Fixture(46, "CustomEventServer GET (deprecated)", "RO", "46"),
    Fixture(52, "PTZ ch1 setup GET",                 "RO", "52"),
    Fixture(54, "V2/Alarm GET (dup of 11)",          "RO", "54"),
    Fixture(58, "NTP GET",                           "RO", "58",
            expect_keys=("ntpEnabled", "ntpServerDomain"), expect_status_ok=False),
    Fixture(61, "timeZone GET",                      "RO", "61"),
    Fixture(62, "SmartDetect/Human GET",             "RO", "62"),
    Fixture(64, "SmartDetect/Motion GET",            "RO", "64"),
    Fixture(66, "SmartDetect/Tamper GET",            "RO", "66"),
    Fixture(68, "User/List GET",                     "RO", "68"),
    Fixture(69, "SD card status2 GET",               "RO", "69"),
    Fixture(73, "NetAPI/System GET",                 "RO", "73"),
    Fixture(75, "Rtmp GET (deprecated NetSdk)",      "RO", "75"),
    Fixture(77, "Protocol/RTSPServer GET",           "RO", "77"),
]

# ---- MUTATING cases (round-trip verified) ----------------------------------
# Each MUT fixture: stimulus = current/safe value; flip_stimulus = different
# value; verify_via_case = which RO case to read back; restore_stimulus =
# put it back to original.
MUT = [
    # encoder ch101 PUT — flip the channelName.  GET-back via case 2.
    Fixture(19, "encoder ch101 PUT (channelName flip)", "MUT",
            stimulus='{"id":101,"channelName":"ARCIS AI","frameRate":15}',
            flip_stimulus='{"id":101,"channelName":"QC-TEST","frameRate":15}',
            verify_via_case=2, verify_key="channelName", verify_value="QC-TEST",
            restore_stimulus='{"id":101,"channelName":"ARCIS AI","frameRate":15}'),

    # video input PUT (image params) — flip brightness 50→55.  GET-back via 6.
    Fixture(20, "video/input PUT (brightness flip)", "MUT",
            stimulus='{"id":1,"brightnessLevel":50}',
            flip_stimulus='{"id":1,"brightnessLevel":55}',
            verify_via_case=6, verify_key="brightnessLevel", verify_value="55",
            restore_stimulus='{"id":1,"brightnessLevel":50}'),

    # /netsdk/image PUT — endpoint exists but verification path is via case 38 GET.
    # Behavior: just check the PUT returns a non-error response.
    Fixture(21, "image PUT (echo)", "MUT",
            stimulus='{"DayNightMode":"Auto"}',
            flip_stimulus='{"DayNightMode":"Auto"}',
            verify_via_case=None),

    # NTP PUT — pool.ntp.org.  GET-back via 58.  Restore: leave at pool.ntp.org.
    Fixture(23, "NTP PUT pool.ntp.org",  "MUT",
            stimulus='{"ntpEnabled":true,"ntpServerDomain":"pool.ntp.org"}',
            flip_stimulus='{"ntpEnabled":true,"ntpServerDomain":"in.pool.ntp.org"}',
            verify_via_case=58, verify_key="ntpServerDomain",
            verify_value="in.pool.ntp.org",
            restore_stimulus='{"ntpEnabled":true,"ntpServerDomain":"pool.ntp.org"}'),

    # motionDetection PUT NetSDK (deprecated — likely fails)
    Fixture(24, "motionDetection PUT (deprecated NetSDK)", "MUT",
            stimulus='{"Enabled":true,"DetectLevel":"Normal"}',
            flip_stimulus='{"Enabled":true,"DetectLevel":"Normal"}',
            verify_via_case=None),

    # V2/Alarm PUT — get-back via 11/54.
    Fixture(25, "V2/Alarm PUT", "MUT",
            stimulus='{"Enabled":true}',
            flip_stimulus='{"Enabled":true}',
            verify_via_case=None),

    # V2/Image/DWDR PUT
    Fixture(26, "V2/Image/DWDR PUT", "MUT",
            stimulus='{"Enabled":false}',
            flip_stimulus='{"Enabled":false}',
            verify_via_case=None),

    # humanDetect PUT (deprecated NetSDK; should be SmartDetect/Human now)
    Fixture(27, "humanDetect PUT (deprecated NetSDK)", "MUT",
            stimulus='{"Enabled":false}',
            flip_stimulus='{"Enabled":false}',
            verify_via_case=None),

    # AI cases (likely firmware-gated; expect not-supported)
    Fixture(28, "AI/FaceDetect PUT (firmware-gated)",      "MUT",
            stimulus='{"Enabled":false}', flip_stimulus='{"Enabled":false}'),
    Fixture(29, "AI/LineCrossDetect PUT (firmware-gated)", "MUT",
            stimulus='{"Enabled":false}', flip_stimulus='{"Enabled":false}'),
    Fixture(30, "AI/HumanCounter PUT",                     "MUT",
            stimulus='{"Enabled":false}', flip_stimulus='{"Enabled":false}'),
    Fixture(31, "AI/RegionDetect PUT (firmware-gated)",    "MUT",
            stimulus='{"Enabled":false}', flip_stimulus='{"Enabled":false}'),
    Fixture(32, "AI/UnattendedObjDetect PUT",              "MUT",
            stimulus='{"Enabled":false}', flip_stimulus='{"Enabled":false}'),
    Fixture(33, "AI/ObjRemoveDetect PUT",                  "MUT",
            stimulus='{"Enabled":false}', flip_stimulus='{"Enabled":false}'),

    # encoder ch102 PUT (sub) — via send_https_request_case40
    Fixture(40, "encoder ch102 PUT (sub)", "MUT",
            stimulus='{"id":102,"frameRate":15}',
            flip_stimulus='{"id":102,"frameRate":15}',
            verify_via_case=39),

    # audio encode PUT
    Fixture(43, "audio/encode ch101 PUT", "MUT",
            stimulus='{"codecType":"AAC"}',
            flip_stimulus='{"codecType":"AAC"}',
            verify_via_case=42),

    # CustomEventServer PUT (deprecated)
    Fixture(45, "CustomEventServer PUT (deprecated)", "MUT",
            stimulus='{"bEnable":false}', flip_stimulus='{"bEnable":false}'),
    Fixture(47, "CustomEventServer PUT (deprecated, dup of 45)", "MUT",
            stimulus='{"bEnable":false}', flip_stimulus='{"bEnable":false}'),

    # SearchRecord PUT (Augentix says "Please update firmware")
    Fixture(48, "R.SearchRecord PUT (firmware-gated)", "MUT",
            stimulus='{"DEV":"IPC","VER":"1.0","API":"R.SearchRecord"}',
            flip_stimulus='{"DEV":"IPC","VER":"1.0","API":"R.SearchRecord"}'),

    # User add (XML-based, needs cam credentials)
    Fixture(49, "User add (XML)", "MUT",
            stimulus='{"username":"qctestuser","password":"qctestpw"}',
            flip_stimulus='{"username":"qctestuser","password":"qctestpw"}',
            verify_via_case=68),
    # User delete
    Fixture(50, "User delete (XML)", "MUT",
            stimulus='{"username":"qctestuser"}',
            flip_stimulus='{"username":"qctestuser"}',
            verify_via_case=68),

    # PTZ control (movement — visible side-effect, brief)
    Fixture(51, "PTZ control (brief)",         "MUT",
            stimulus='{"command":"up"}', flip_stimulus='{"command":"up"}'),

    # PTZ setup PUT + cruise control
    Fixture(53, "PTZ setup PUT (cruise)",      "MUT",
            stimulus='{"cruiseMode":"stop"}', flip_stimulus='{"cruiseMode":"stop"}'),

    # V2/Alarm PUT
    Fixture(55, "V2/Alarm PUT (dup of 25)",    "MUT",
            stimulus='{"Enabled":true}', flip_stimulus='{"Enabled":true}'),

    # timeZone PUT — verify via 61.
    Fixture(59, "timeZone PUT IST",  "MUT",
            stimulus='"GMT+05:30"',
            flip_stimulus='"GMT+05:30"',
            verify_via_case=61),

    # localTime PUT (sets manual time)
    Fixture(60, "localTime PUT",     "MUT",
            stimulus='"2026-05-19T15:00:00+05:30"',
            flip_stimulus='"2026-05-19T15:00:00+05:30"',
            verify_via_case=9),

    # SmartDetect/Human PUT — flip Enabled false→true, restore.  GET-back via 62.
    Fixture(63, "SmartDetect/Human PUT (Enabled flip)", "MUT",
            stimulus='{"Enabled":false,"DetectLevel":"Inherit"}',
            flip_stimulus='{"Enabled":true,"DetectLevel":"Inherit"}',
            verify_via_case=62, verify_key="Enabled", verify_value="True",
            restore_stimulus='{"Enabled":false,"DetectLevel":"Inherit"}'),

    # SmartDetect/Motion PUT (round-trip)
    Fixture(65, "SmartDetect/Motion PUT (Enabled flip)", "MUT",
            stimulus='{"Enabled":true,"DetectLevel":"Normal"}',
            flip_stimulus='{"Enabled":true,"DetectLevel":"High"}',
            verify_via_case=64, verify_key="DetectLevel", verify_value="High",
            restore_stimulus='{"Enabled":true,"DetectLevel":"Normal"}'),

    # SmartDetect/Tamper PUT (round-trip)
    Fixture(72, "SmartDetect/Tamper PUT", "MUT",
            stimulus='{"IPCDetect":{"Enabled":true}}',
            flip_stimulus='{"IPCDetect":{"Enabled":false}}',
            verify_via_case=66, verify_key="Enabled", verify_value="False",
            restore_stimulus='{"IPCDetect":{"Enabled":true}}'),

    # preset.cgi (CGI-based)
    Fixture(70, "preset.cgi (CGI)", "MUT",
            stimulus='{"status":1,"act":"set","number":1}',
            flip_stimulus='{"status":1,"act":"set","number":1}'),

    # password change (XML — sensitive: skip-by-default; tests path)
    Fixture(71, "user password change (XML)", "MUT",
            stimulus='{"old_pass":"admin","new_pass":"admin"}',
            flip_stimulus='{"old_pass":"admin","new_pass":"admin"}'),

    # NetAPI/System PUT (round-trip)
    Fixture(74, "NetAPI/System PUT", "MUT",
            stimulus='{}', flip_stimulus='{}', verify_via_case=73),

    # Rtmp PUT (deprecated; should be RTMPClinet now)
    Fixture(76, "Rtmp PUT (deprecated NetSdk)", "MUT",
            stimulus='{"Enabled":false,"Stream":1,"Url":""}',
            flip_stimulus='{"Enabled":false,"Stream":1,"Url":""}',
            verify_via_case=75),

    # RTSPServer PUT (round-trip)
    Fixture(78, "Protocol/RTSPServer PUT", "MUT",
            stimulus='{"Enable":true,"EnableTLS":false}',
            flip_stimulus='{"Enable":true,"EnableTLS":false}',
            verify_via_case=77),
]

# ---- STUB cases: deliberately return "not supported in your product" -------
STUB = [
    Fixture(3,  "stub: not-supported",  "STUB_NS", "3"),
    Fixture(12, "stub: not-supported",  "STUB_NS", "12"),
    Fixture(13, "stub: not-supported",  "STUB_NS", "13"),
    Fixture(14, "stub: not-supported",  "STUB_NS", "14"),
    Fixture(15, "stub: not-supported",  "STUB_NS", "15"),
    Fixture(16, "stub: not-supported",  "STUB_NS", "16"),
    Fixture(17, "stub: not-supported",  "STUB_NS", "17"),
    Fixture(18, "stub: not-supported",  "STUB_NS", "18"),
    Fixture(22, "stub: not-supported",  "STUB_NS", "22"),
    Fixture(44, "stub: not-supported",  "STUB_NS", "44"),
]

# ---- STATE-management special cases (no normal request/response) -----------
STATE = [
    Fixture(0,  "keepalive / status tracker", "STATE", "status: online"),
    Fixture(1,  "p2p ini file push",          "STATE", "[dummy_ini_content]"),
    Fixture(56, "websocket audio stream",     "STATE", "test"),
    Fixture(57, "buzzer / two-way audio",     "STATE", ""),
    Fixture(80, "credential sync (CAM_USER/PASS)", "STATE",
            '{"username":"admin","password":""}'),
]

# ---- DESTRUCTIVE -----------------------------------------------------------
DEST = [
    # case 36 OTA — testable safely: send a deliberately bad URL.  Camera must
    # reject with "Invalid OTA URL" and NOT actually download/flash.
    Fixture(36, "OTA — invalid URL must reject", "DEST_TESTABLE",
            stimulus="ftp://example.com/bad",
            must_reject_with="Invalid OTA URL"),
    # case 41 reboot — runs LAST.  Verified by camera coming back live.
    Fixture(41, "system reboot — runs LAST",     "DEST_REBOOT",
            stimulus="{}"),
    # case 67 SD format — opt-in only.
    Fixture(67, "SD card format — opt-in only",  "DEST_OPTIN",
            stimulus="{}"),
]

# ---- v2 retained config — handled in mqtt_thread.c -------------------------
V2 = [
    Fixture(81, "retained config (handled in mqtt_thread)", "V2", ""),
]

# ---- master ordered list ---------------------------------------------------
def all_fixtures() -> list[Fixture]:
    return sorted(RO + MUT + STUB + STATE + DEST + V2, key=lambda f: f.n)
