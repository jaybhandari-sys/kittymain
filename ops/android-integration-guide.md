# Android integration guide — kitty MQTT + video, step by step

**Audience:** the Android engineer integrating the ArcisAI mobile app
against a kitty-flashed Augentix camera.  Covers the failure modes we
have actually seen on bench builds, in the order they bite.

**Pair with:** [`ops/dev-integration-guide.md`](dev-integration-guide.md)
for the generic protocol description.  This doc is the Android-specific
recipe + the "MQTT keeps timing out on the APK" cookbook.

If you only have 60 seconds, skip to [§3](#3-android-mqtt-timeout-the-7-common-causes).

---

## 1. The stack — what your APK actually talks to

```
  ┌──────────────────────────────────────────────────────────────────┐
  │  Android APK                                                     │
  │                                                                  │
  │   ┌────────────────┐    ┌────────────────────────────────────┐   │
  │   │  Paho MQTT     │    │  STUN + signaling + SRT + ExoPlayer │   │
  │   │  (control)     │    │  (live video pipeline)              │   │
  │   └────────┬───────┘    └────────────────────────────────────┘   │
  └────────────┼──────────────────┬──────────────────┬───────────────┘
               │ TCP/MQTT         │ UDP STUN/SRT     │ TCP signaling
               ▼                  ▼                  ▼
   mqtt-staging.devices.arcisai.io:443    turn.devices.arcisai.io:5349    signaling.devices.arcisai.io:80
   (plain MQTT, no TLS)                  (UDP)                            (plain TCP, NOT HTTP)
               │
   broker keeps two retained topics per device:
     torque/rx/<SN>/90  ← device profile (camera reads on subscribe)
     torque/tx/<SN>/boot ← device boot summary (your APK reads to verify camera is up)

   Per-case request/response (every kitty case in src/ap2p/legacy_cases.c):
     PUBLISH  to torque/rx/<SN>/<N>     (consumer → camera)
     SUBSCRIBE  torque/tx/<SN>/<N>      (camera → consumer)
```

Five facts the rest of this guide leans on:

1. **MQTT broker is plain TCP on port 443.**  `tcp://…:443`, **not** `ssl://`, **not** `mqtts://`, **not** WebSocket.  Port 443 is just there because corporate firewalls never block it — the protocol on the wire is unencrypted MQTT v3.1.1.  Most timeout reports we've seen come from connecting with the wrong scheme.
2. **No cleartext = no MQTT on Android 9+.**  By default Android API 28+ refuses non-TLS network sockets.  Without an explicit opt-in in `AndroidManifest.xml` or a `network_security_config.xml` the broker connection will silently fail with a timeout, never a useful error.
3. **Subscribe BEFORE publish.**  The camera's response lands on `torque/tx/<SN>/<N>` within ~200 ms.  If you publish first and subscribe second, the response is gone before your subscription is in place.
4. **The /90 retained payload exists per device.**  A camera fresh out of the box does NOT have it — the operator runs `flash-kitty.py --provision-90` once.  Without /90 the camera boots, connects to MQTT, sees nothing on /rx/<SN>/90, and blocks on `state_ready_cv` forever.  Your APK can verify this with one `mosquitto_sub`.
5. **Last-viewer-wins on the camera.**  When more than one consumer hits the same SRT channel, the older pump is cancelled.  Two test apps fighting for the same camera = both flap.  Pick `live_ch0_0.flv` (main, 2304×1296) or `live_ch0_1.flv` (sub, 800×448).

---

## 2. End-to-end smoke test from your laptop (5 min)

**Do this first.**  If it fails, the broker / camera is the problem and there's no point debugging the APK.

```bash
# Prerequisites: brew install mosquitto

SN=ATPL-508600-AUGEN          # change to the camera you flashed

# 1. Is the camera alive on the broker?
mosquitto_sub -h mqtt-staging.devices.arcisai.io -p 443 \
              -u Torque -P 'Raptor@0' \
              -t "torque/tx/$SN/boot" -C 1 -W 8 -v

# Expected: one JSON line within 1-2 s containing "node_id":"ATPL-...",
# "version":"2.0.x", and a "since_ambicam_start_sec" object whose
# ctrl_registered field is non-negative.
# If "Timed out" — the camera has not published a fresh boot yet.

# 2. Is the /90 retained payload there?
mosquitto_sub -h mqtt-staging.devices.arcisai.io -p 443 \
              -u Torque -P 'Raptor@0' \
              -t "torque/rx/$SN/90" -C 1 -W 6

# Expected: 17 lines starting with NODE_ID=, CTRL_HOST=, etc.
# If empty: re-run flash-kitty.py --provision-90, or
#   tools/flash-kitty.py --ip <ip> --tag <last>  (will publish /90).

# 3. Exercise one case round-trip (case 8 = deviceInfo, read-only safe).
mosquitto_sub -h mqtt-staging.devices.arcisai.io -p 443 \
              -u Torque -P 'Raptor@0' \
              -t "torque/tx/$SN/8" -v &
SUB=$!
sleep 1
mosquitto_pub -h mqtt-staging.devices.arcisai.io -p 443 \
              -u Torque -P 'Raptor@0' \
              -t "torque/rx/$SN/8" -m '{}'
sleep 3
kill $SUB

# Expected within 1 s: one JSON response on /tx/<SN>/8 with
# "deviceName", "model", "macAddress", "serialNumber", etc.
```

All three pass → the broker + camera are fine; the issue is in the APK.

---

## 3. Android MQTT timeout — the 7 common causes

In the order we've actually hit them on bench Android builds.  **#1 + #2 cover ~80% of "MQTT timed out" reports.**

### 3.1 Wrong URI scheme  ← most common

```kotlin
// ❌ Wrong — these all silently time out at ~30 s
val brokerUri = "ssl://mqtt-staging.devices.arcisai.io:443"
val brokerUri = "mqtts://mqtt-staging.devices.arcisai.io:443"
val brokerUri = "wss://mqtt-staging.devices.arcisai.io:443"

// ✅ Correct — plain TCP, port 443 happens to be the listener
val brokerUri = "tcp://mqtt-staging.devices.arcisai.io:443"
```

### 3.2 Cleartext blocked by default (Android 9, API 28+)

If you see something like this in logcat:

```
W/System.err: javax.net.ssl.SSLHandshakeException: ...
  → CleartextNotPermittedException: Cleartext traffic to mqtt-staging.devices.arcisai.io not permitted
```

…or just a silent timeout, you need to opt-in to cleartext.

**Quick fix (everything cleartext, OK for dev builds):**

```xml
<!-- AndroidManifest.xml -->
<application
    android:usesCleartextTraffic="true"
    ...>
```

**Production-safe fix (cleartext only for the broker domain):**

```xml
<!-- res/xml/network_security_config.xml -->
<?xml version="1.0" encoding="utf-8"?>
<network-security-config>
    <domain-config cleartextTrafficPermitted="true">
        <domain includeSubdomains="false">mqtt-staging.devices.arcisai.io</domain>
        <domain includeSubdomains="false">signaling.devices.arcisai.io</domain>
    </domain-config>
</network-security-config>
```

```xml
<!-- AndroidManifest.xml -->
<application
    android:networkSecurityConfig="@xml/network_security_config"
    ...>
```

### 3.3 MqttConnectOptions defaults are too conservative

Paho's stock defaults give 30 s timeout, no auto-reconnect, MQTT v3.1 (not v3.1.1).  Use these:

```kotlin
val opts = MqttConnectOptions().apply {
    isCleanSession      = true
    isAutomaticReconnect = true              // critical — Wi-Fi blips happen
    connectionTimeout    = 15                // s; default 30, too long for UX
    keepAliveInterval    = 30                // s; broker drops at 60
    mqttVersion          = MqttConnectOptions.MQTT_VERSION_3_1_1
    userName             = "Torque"
    password             = "Raptor@0".toCharArray()
    // ↓ ONLY if you ever switch to ssl:// — leave commented for tcp://
    // socketFactory     = TLSSocketFactory(trustAllCerts)
}
```

### 3.4 Calling `MqttClient.connect()` on the main thread

Paho's synchronous `MqttClient` blocks until connect succeeds OR times out.  If you call it on Activity#onCreate, the system kills the app with `ANR` before the broker even answers.  Either:

* use `MqttAsyncClient` (callback-based, never blocks the caller), or
* wrap `MqttClient.connect()` in a coroutine on `Dispatchers.IO`.

```kotlin
lifecycleScope.launch(Dispatchers.IO) {
    try {
        mqttClient.connect(opts)
    } catch (e: MqttException) {
        Log.e("MQTT", "connect failed", e)
    }
}
```

### 3.5 Subscribing AFTER publishing

The camera answers within ~200 ms.  This pattern misses every response:

```kotlin
// ❌ race — subscribe happens after the response is already in flight
client.publish("torque/rx/$sn/8", "{}".toByteArray(), 1, false)
client.subscribe("torque/tx/$sn/8")          // too late
```

Subscribe first, then publish:

```kotlin
// ✅
client.subscribe("torque/tx/$sn/8") { topic, msg ->
    Log.d("MQTT", "case 8 reply: ${String(msg.payload)}")
}
// give the SUBACK ~50 ms to land on the broker before the publish
delay(50)
client.publish("torque/rx/$sn/8", "{}".toByteArray(), 1, false)
```

For a control-plane app, subscribe to the wildcard ONCE at app start:

```kotlin
client.subscribe("torque/tx/$sn/#") { topic, msg ->
    val caseNum = topic.substringAfterLast("/")
    routeResponse(caseNum, msg.payload)
}
```

### 3.6 Topic wildcard wrong

* `+` matches one level.  `torque/tx/+/boot` = every device's boot.  Use to discover online devices.
* `#` matches multi-level, must be terminal.  `torque/tx/$sn/#` = every response from this device.
* You **cannot** publish to a wildcard.  Publishing to `torque/rx/$sn/+` silently fails.

### 3.7 Camera has no /90 retained yet

If a fresh camera has never been provisioned, the broker holds nothing on `torque/rx/<SN>/90`.  ap2p subscribes on boot, sees no retained payload, and blocks on `state_ready_cv` waiting forever.  Your APK can connect to the broker fine — it'll just never see any responses because ap2p hasn't reached the case-dispatch loop.

Verify with the smoke test in §2 step 2.  Fix:

```bash
tools/flash-kitty.py --ip <camera_ip> --tag <last> --provision-90
# or
ops/fleet-provision.sh fleet.csv
```

---

## 4. Minimal working Paho-Android example

Drop into a fresh module, change `SN`, hit run.  Targets API 24+.

### Gradle

```kotlin
// app/build.gradle.kts
dependencies {
    implementation("org.eclipse.paho:org.eclipse.paho.client.mqttv3:1.2.5")
    implementation("org.eclipse.paho:org.eclipse.paho.android.service:1.1.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
}
```

### Manifest (cleartext + service)

```xml
<application
    android:usesCleartextTraffic="true"
    ... >
    <service android:name="org.eclipse.paho.android.service.MqttService"/>
</application>

<uses-permission android:name="android.permission.INTERNET"/>
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE"/>
<uses-permission android:name="android.permission.WAKE_LOCK"/>
```

### Kotlin

```kotlin
import org.eclipse.paho.client.mqttv3.*
import org.eclipse.paho.android.service.MqttAndroidClient
import kotlinx.coroutines.*

class KittyClient(ctx: Context, private val sn: String) {
    private val client = MqttAndroidClient(ctx,
        "tcp://mqtt-staging.devices.arcisai.io:443",
        "android-${sn}-${System.currentTimeMillis()}")    // unique clientId

    private val responses = MutableSharedFlow<Pair<String, ByteArray>>(extraBufferCapacity = 32)
    val responseFlow = responses.asSharedFlow()

    fun connect(onReady: (Boolean) -> Unit) {
        val opts = MqttConnectOptions().apply {
            isCleanSession       = true
            isAutomaticReconnect = true
            connectionTimeout    = 15
            keepAliveInterval    = 30
            mqttVersion          = MqttConnectOptions.MQTT_VERSION_3_1_1
            userName             = "Torque"
            password             = "Raptor@0".toCharArray()
        }
        client.setCallback(object : MqttCallback {
            override fun messageArrived(topic: String, msg: MqttMessage) {
                responses.tryEmit(topic to msg.payload)
            }
            override fun connectionLost(cause: Throwable?) {
                Log.w("MQTT", "lost: $cause")
                // Paho will auto-reconnect (isAutomaticReconnect=true).
            }
            override fun deliveryComplete(token: IMqttDeliveryToken?) {}
        })
        client.connect(opts, null, object : IMqttActionListener {
            override fun onSuccess(token: IMqttToken?) {
                client.subscribe("torque/tx/$sn/#", 1)     // all camera responses
                onReady(true)
            }
            override fun onFailure(token: IMqttToken?, ex: Throwable?) {
                Log.e("MQTT", "connect FAILED", ex)
                onReady(false)
            }
        })
    }

    /** Fire a request (publish to /rx/<SN>/<case>).  Responses arrive
     *  on responseFlow filtered by topic (/tx/<SN>/<case>). */
    fun sendCase(case: Int, payload: ByteArray = "{}".toByteArray()) {
        client.publish("torque/rx/$sn/$case", payload, 1, false)
    }

    fun disconnect() {
        try { client.disconnect() } catch (_: Exception) {}
    }
}

// Usage in an Activity / ViewModel:
class KittyViewModel : ViewModel() {
    fun start(ctx: Context, sn: String) = viewModelScope.launch(Dispatchers.IO) {
        val kc = KittyClient(ctx, sn)
        kc.connect { ok ->
            if (!ok) { Log.e("UI", "MQTT connect failed"); return@connect }
            // Subscribe to specific response in the UI thread via the flow:
            viewModelScope.launch {
                kc.responseFlow
                  .filter { it.first.endsWith("/8") }
                  .collect { (_, body) ->
                      val info = String(body)
                      Log.d("UI", "deviceInfo: $info")
                  }
            }
            kc.sendCase(8)              // ask the camera for deviceInfo
        }
    }
}
```

---

## 5. Video pipeline on Android — short version

The MQTT control plane and the video plane are **independent**.  MQTT is for "show me settings / change settings / reboot the camera".  Video is the STUN → signaling → SRT → HTTP-FLV chain documented in
[`ops/dev-integration-guide.md`](dev-integration-guide.md#3-end-to-end-flow-your-consumer-must-implement).

Android-specific notes:

* **STUN/TURN** is UDP — works fine over carrier 4G and most WLANs.  If your test wifi blocks UDP outbound, use `turn.devices.arcisai.io` as a TURN relay (same host).
* **libsrt for Android** ships from <https://github.com/Haivision/srt>; build with `cmake -DENABLE_HEVC=ON -DENABLE_CXX_DEPS=OFF`.  Set `SRTO_TRANSTYPE=SRTT_FILE`, **not** `SRTT_LIVE`, or the handshake errors with "Agent uses MESSAGE API…".
* **FLV header patch** is mandatory.  The camera writes `DataOffset=0` (spec says 9).  In Java:

  ```kotlin
  // After receiving the first 9 bytes from SRT:
  buf[5] = 0; buf[6] = 0; buf[7] = 0; buf[8] = 9   // big-endian uint32 = 9
  ```

* **HEVC in FLV** uses `videoCodecId = 12` (CN-IPC convention).  ExoPlayer's stock FLV extractor does not handle this — wrap with [`com.google.android.exoplayer2.source.MediaSource`](https://exoplayer.dev/) or feed raw HEVC NALs you've parsed yourself from the FLV tags.
* **Last viewer wins**.  When the user backgrounds the app and someone else connects to the same camera, your SRT pump dies.  Subscribe to `torque/tx/<sn>/boot` and treat any fresh boot publish as "camera restarted, reconnect SRT".

---

## 6. Troubleshooting flowchart

```
APK reports "MQTT timed out"
│
├── Does `mosquitto_sub` from §2 work?
│   └── No → camera/broker problem, not APK.
│           Re-flash and re-provision the camera.
│
├── Yes → APK problem.  Check in order:
│
├── Does logcat show "CleartextNotPermittedException"?
│   └── Yes → add usesCleartextTraffic=true (§3.2).
│
├── Is broker URI "ssl://" or "mqtts://"?
│   └── Yes → change to "tcp://..." (§3.1).
│
├── Are you on Android 9+ AND not using Service?
│   └── Use MqttAndroidClient with the foreground Service (§4),
│      or wrap MqttClient in Dispatchers.IO coroutine.
│
├── Is connectionTimeout default (30)?
│   └── If user perceives "no response", lower to 15s so the UI can
│      show "broker unreachable" instead of seeming frozen.
│
├── Did you subscribe AFTER publishing?
│   └── Yes → subscribe first, wait 50 ms, then publish (§3.5).
│
└── Still timing out?
    └── tcpdump on the AP / laptop tethered to phone, look for
       SYN-RETRANSMIT or RST.  Likely corp firewall blocking 443
       outbound (rare but happens).
```

---

## 7. Common case payloads (quick reference)

Cases your APK is most likely to send.  Full list in
[`docs/MQTT_CASES.md`](../docs/MQTT_CASES.md).

| Case | Direction | Payload (rx) | What you get on tx | Notes |
|---:|:--|:--|:--|:--|
| 1  | RO | `{}` | INI file contents (provider config) | Useful for confirming retained-config applied |
| 2  | RO | `{}` | Main-stream encoder JSON | resolution, bitrate, gop |
| 6  | RO | `{}` | Image params | brightness, contrast, saturation |
| 7  | RO | `{}` | `{ "macAddress": "AA:BB:CC:DD:EE:FF" }` | MAC quick-read |
| 8  | RO | `{}` | full `/netsdk/system/deviceinfo` JSON | deviceName, model, serialNumber (=BurnUID), macAddress, firmwareVersion |
| 19 | MUT | encoder config JSON | echoed config | Set main stream |
| 20 | MUT | image params JSON | echoed config | Set image |
| 38 | RO | `{}` | image config | |
| 39 | RO | `{}` | Sub-stream encoder JSON | resolution, bitrate, gop |
| 41 | DEST | `{}` | reboot ACK | **WILL REBOOT THE CAMERA** — use sparingly |
| 58 | RO | `{}` | NTP config | `{ "ntpEnabled": true, "ntpServerDomain": "..." }` |
| 61 | RO | `{}` | TZ string | `"GMT+05:30"` |
| 66 | RO | `{}` | tamper-detect status | replaces /81 (tampering) on v2.0.7+ |
| 80 | RO | `{}` | network status (interfaces, IPs, signal) | from `/NetAPI/R.Sync.Stat.NetWork` |
| 90 | (retained config, do NOT publish — broker stores 17-key payload here from operator-side fleet-provision) | | | |

Avoid these from the APK without a UI confirmation: **23 (NTP set)**,
**36 (OTA download + reboot)**, **41 (reboot)**, **67 (SD format)**.

---

## 8. What still won't work on the APK today

Same caveats as `dev-integration-guide.md` §6:

* **NTP set via case 23** silently returns 200 but the vendor firmware on v6.0.13.578021 doesn't persist `ntpServerDomain`.  Until the camera-side `ambicam.sh` patch lands (planned v2.0.7-rc3), the OSD clock can drift several seconds per hour.  Visible in the boot publish: `state_ready` is correct, but the wall clock the camera stamps onto frames is off.
* **Production cloud (GCP) is read-only.**  Don't write to broker topics outside `torque/{rx,tx}/<your-SN>/#`.  The QC tooling at the bench shares this broker.

---

## 9. Versions you should pin in your build

* Paho Java MQTT v3: **1.2.5** (last release; service shim 1.1.1)
* libsrt: **1.5.4** (matches camera-side; older versions hit a stream-mode handshake bug)
* AGP / Gradle: anything ≥ 7.4
* compileSdk ≥ 33; `usesCleartextTraffic` or network_security_config required

That's the whole stack.  If §2's three commands pass and your APK still
times out, paste the logcat dump (everything between MQTT connect attempt
and the timeout) into a ticket; the cause is almost always one of §3.1–§3.7
and a logcat line names it.
