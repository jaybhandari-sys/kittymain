# Roadmap — v2.0.4 → v2.1.0 (production + manufacturing grade)

> **Status:** v2.0.7-rc1 shipped (MQTT pipeline fix — biggest gap, found via
> independent QC report).  Numbering shifted down: TRUE OTA is now v2.0.8,
> joint BSP-OTA v2.0.9, QC-alignment + manufacturing v2.1.0.
>
> **North star:** v2.1.0 is the first "ship to manufacturing" tag.
> Every release in this file is a step toward that: faster, more
> robust, more verifiable, fully QC'd against the ArcisAI Kitty
> QC Tool (`github.com/hardiksa/CameraUtilityTool`).

## Where we are today (v2.0.4-rc1 baseline)

On `ATPL-200007-TESTA`, kernel boot → live measured across 2 cycles:

| | Cycle 1 | Cycle 2 | Notes |
|---|---:|---:|---|
| Camera-internal: launcher → live | 21.3 s | 22.0 s | dominated by ↓ |
| └── mqtt_connecting → mqtt_connected | 17.5 s | 17.6 s | **81 % of path** |
| Kernel uptime at live | 27.7 s | 28.4 s |  |
| Host-observed: reboot → live | 35.1 s | 35.9 s | premium-IPC target ✅ |

The 17.5 s gap is the dominant cost.  Root cause: `ap2p` exec'd before
wlan0 + DHCP + routing were fully usable; first MQTT TCP connect
timed-out; old retry policy slept 5 s × 3 = 15+ s before a successful
attempt.

Every other phase (config parse, subscribe, retained config, STUN, signaling
REGISTER) totals < 4 s.  **The whole optimization story below is about
attacking that 17.5 s window once, then never again.**

---

## v2.0.5 — fast boot path  *(implemented, CI building)*

**Goal:** collapse the 17.5 s broker-connect stall to < 1 s.

Changes shipped on `v2.0.5-rc1`:

1. **`ambicam.sh.v2` network-ready gate.**  Before `exec`-ing `ap2p`, poll
   `/proc/net/route` for a default-route entry every 200 ms (cap 30 s).
   When the camera comes back from reboot, `ap2p` does NOT run until
   wlan0 actually has a usable IP + gateway.  Cost when network is ready
   instantly: zero.  Cost when network is delayed: replaces a 5 s sleep
   loop in `connect_once()` with a tighter 200 ms poll outside `ap2p`.

2. **`mqtt_thread.c` timeline-based retry.**  For the first 30 s of
   process lifetime, the MQTT connect retry interval is 500 ms instead
   of 5 s.  Three retries cost 1.5 s instead of 15 s.  After the
   30 s window, fall back to the original cadence so a genuinely-down
   broker doesn't pin the CPU.

3. Also tightened the `/mny/mtd/ipc/ambicam/` wait from 2 s polls
   (60 s cap) to 200 ms polls (15 s cap).

**Expected post-deploy numbers** (modeled from v2.0.4 data):

| Metric | v2.0.4-rc1 | v2.0.5 target |
|---|---:|---:|
| launcher → live | 21.6 s | **~5 s** |
| kernel boot → live | 28.1 s | **~13 s** |
| host-observed: reboot → live | 35.5 s | **~18 s** |

**Verification:** same 2-cycle measurement harness as v2.0.4 (auto via
`/tmp/measurement_v2.0.4-rc1_ATPL-200007-TESTA.*` template).  Camera
publishes the boot-timing JSON to `torque/tx/<sn>/boot` — no telnet needed.

**Owner:** user deploys v2.0.5-rc1 after CI/CD completes.

---

## Server-side hardening + failover + DR  *(parallel track, v2.0.5.x → v2.1.0)*

**Goal:** the camera survives a single broker / signaling / TURN failure
without operator intervention.  Production fleet runs across at least
two availability zones; one zone going down loses no cameras.

This is **NOT a single release**; it's a track that lands incrementally:

### Camera-side wiring  *(landed on `feat/v2-ap2p`, post-`v2.0.5-rc1`)*

- `state.h` carries `bootstrap_mqtt_url_bkp` next to the primary URL.
- `config.c` bootstrap loader extracts `mqttUrl_BKP` from
  `/mny/mtd/ipc/ambicam/config.json` (empty → no failover).
- `mqtt_thread.c` outer loop alternates primary ↔ backup on failure.
  After a successful connect the URL becomes "sticky" so a single bad
  broker doesn't ping-pong sessions.
- Same fast-retry policy as v2.0.5 applies to both URLs — first 30 s
  is aggressive against both, then settled cadence.

### Signaling failover  *(v2.0.6.x parallel)*

- `CTRL_HOST` becomes `CTRL_HOST` + `CTRL_HOST_BKP` in the retained
  case-81 payload.  SRT thread alternates the same way MQTT does.
- TURN / STUN: `EDGE_HOST_BKP` + `RELAY_HOST_BKP` likewise.
- All extensions are backward-compatible — payloads missing the
  `_BKP` keys behave exactly like v2.0.5.

### Server-side hardening  *(coordinated with infra, blocks v2.1.0)*

- Stand up `mqtt-eu-staging.devices.arcisai.io` in a 2nd GCP region.
  Mosquitto bridge mode replicates retained messages.  Drift target
  ≤ 5 s under normal load.
- Same for `signaling-eu.devices.arcisai.io` (etcd-backed peer list
  shared between the two).
- TURN: anycast OR per-region (cameras try both via the `_BKP` fields).
- DR runbook (`docs/DR_RUNBOOK.md`) covers: region failover, secret
  rotation, fleet re-provision via `ops/fleet-provision.sh`, broker
  restore from S3 retained-snapshot dumps.
- Monitor: per-broker `$SYS/broker/clients/active` alerted at < 80 %
  of fleet size; failed-publish rate alerted at > 1/min/camera.

### v2.1.0 release criterion: kill-broker test

- Take down `mqtt-staging.devices.arcisai.io` in firewall.  Within
  60 s, ≥ 90 % of the fleet must be live on the backup broker
  (verifiable via `mosquitto_sub -t 'torque/tx/+/boot'` on backup).
- Bring main back; cameras stay on backup until they next reconnect
  (sticky), then drift back over the next hour as their TCP sessions
  rotate.  No fleet-wide thundering herd.

### Out-of-scope for v2.1.0 (planned in v3.0+)

- Active-active broker with sub-second drift.
- Camera-side preference learning (RTT-based broker selection).
- Geographic broker pinning by camera serial range.

---

## v2.0.6 — video stream startup latency  *(planning)*

**Goal:** characterize and reduce the ~2 s delay between viewer-connect and
first-frame-display.

**Today's video path** (one-way, camera → viewer):

```
camera sensor → ISP → H.264 enc → local FLV mux → camera's HTTP-FLV server
   → ap2p HTTP-source pump (libcurl)   →  ap2p SRT TX
   → SRT NAT-traversal tunnel          →  viewer (augentix_view.py)
   → FLV demux → H.264 dec             →  ffplay display
```

The 2 s delay could be any of:

1. **Camera-side FLV server** waiting for next I-frame before serving bytes
   (typical GOP = 1 s; first-byte wait can be up to 1 GOP).
2. **ap2p HTTP pump** TCP-connect + HTTP GET round-trip (~50 ms local).
3. **SRT tunnel setup** — rendezvous handshake (200–500 ms typical;
   3 + 1 way after ICE candidate exchange).
4. **Viewer-side prebuffer** — ffplay's `-fflags nobuffer`
   not yet set; default prebuffer is ~500 ms-1 s.
5. **Encoder first-IDR latency** if the encoder pauses on idle.

**Instrumentation plan** (mirrors v2.0.4 boot-timing pattern):

1. New milestone enum in `boot_timing.h` *(or a separate
   `stream_timing.{c,h}` module to keep concerns separate)*:
   - `ST_VIEWER_CONNECT_RX`   — signaling sees a viewer connect
   - `ST_SRT_HANDSHAKE_START` — local SRT socket connects to viewer
   - `ST_SRT_HANDSHAKE_DONE`  — SRT session established
   - `ST_HTTP_GET_OPEN`       — TCP to camera HTTP-FLV opened
   - `ST_HTTP_FIRST_BYTE`     — first byte read from local HTTP
   - `ST_FIRST_FLV_PUSHED`    — first SRT packet sent to peer
2. Publish per-session JSON to `torque/tx/<sn>/stream/<viewer_id>`.
3. **Viewer-side correlation:** `augentix_view.py` stamps `t_start`,
   `t_sig_register_ack`, `t_srt_handshake_done`, `t_first_flv_byte`,
   `t_first_decoded_frame`, writes to `/tmp/viewer_session.json`.
4. NTP-sync host + camera clocks to ±50 ms; use kernel `CLOCK_MONOTONIC`
   on the camera + wall clock cross-stamped at boot for translation.

**Expected outcome:** identify which of the 5 stages owns the 2 s.  Fix
the dominant one in v2.0.6, the next in a v2.0.6.1 if needed.

**Acceptance:** `view-feed.sh` shows first decoded frame ≤ 500 ms after
`SRT up, FLV flowing` log line.

---

## v2.0.7 — true OTA  *(planning)*

**Goal:** replace the manual `wget Augentix.tar.gz → tar -xzf → cp` recipe
(which requires telnet) with a self-driving OTA flow that runs end-to-end
without any human shell access.

**Today's "OTA" is actually a manual flash:**

```
operator → telnet camera → wget Augentix.tar.gz → tar/cp → reboot
```

That's fine for the dev unit; not acceptable for fleet rollout.

**v2.0.7 true OTA design** (mirrors the v1.13.x `case 36` OTA path that
already exists in `legacy_cases.c:1182-1302`, hardened):

1. **Trigger via MQTT case 36** — operator publishes a JSON payload to
   `torque/rx/<sn>/36` containing:
   ```json
   {
     "url":       "https://prong.arcisai.io/protected/Augentix/A_Series_2.0.7/Augentix.tar.gz",
     "sha256":    "<hex>",
     "signature": "<base64 ed25519 sig over the sha256>",
     "rollback":  "v2.0.6"
   }
   ```
2. **`ap2p` validates** signature against an embedded public key (already
   the v2.1 mTLS-cert pubkey, or a separate OTA-signing key).  Reject
   on hash mismatch.
3. **Atomic staging:** download to `/mny/mtd/ipc/staging/` (a fresh
   subdir), then `mv` directories in one syscall.  Pre-OTA state
   preserved in `/mny/mtd/ipc/ambicam.prev/` for rollback.
4. **Reboot via PUT `/netsdk/system/operation/reboot`** (we already
   know this endpoint works).
5. **Post-OTA health check:** new `ap2p` publishes `torque/tx/<sn>/ota`
   on first successful signaling REGISTER.  If broker doesn't see it
   within 5 minutes, fleet management triggers rollback (publishes
   case 36 again with the prev tarball URL).
6. **Manifest version pinning** — `/mny/mtd/ipc/ambicam/VERSION` file
   is the source of truth.  OTA refuses to install a tarball with
   `VERSION` ≤ current installed version (anti-replay).

**Constraints:**
- Total OTA download + apply window: < 3 minutes per camera.
- Rollback path tested EVERY release (CI automation runs the rollback
  scenario against the dev camera).
- No reboot loop possible: if v2.0.7 + 1 boot attempt fails to publish
  case-36 ack within 90 s, ambicam.sh auto-restores `ambicam.prev/`
  and reboots once.  If THAT also fails, sit in safe mode (legacy
  v1.13.x layout still on disk).

**Acceptance:**
- Manual flash recipe stays as a documented fallback.
- 10 OTA cycles on `ATPL-200007-TESTA` with zero manual intervention.
- One forced rollback cycle (publish corrupt sha256) recovers cleanly.

---

## v2.0.8 — base firmware update  *(scoping)*

**Goal:** integrate Augentix-vendor base firmware updates with our OTA
chain so the whole stack (BSP + kernel + drivers + ap2p) can be rolled
together.

**This is the hardest stage.**  The Augentix BSP / kernel / busybox
rootfs is vendor-owned; we can't rebuild it.  But we CAN integrate.

**What Augentix provides:**
- Periodic `Augentix_BSP_*.rom` images (full-rootfs flash via U-Boot)
- `/netsdk/system/operation/upgrade` API endpoint
  (documented at API checklist item 37 — see `Camera_API_Testing_Checklist`)
- A signing / verification model (TBD with Augentix; ask before
  shipping)

**v2.0.8 deliverables:**

1. **Embed `ap2p` in the Augentix BSP image.**  Work with the vendor to
   include `Augentix.tar.gz` contents at a known path inside their
   per-build `Augentix_BSP_*.rom`.  Single-image flash gives single
   golden state — no race between BSP version and `ap2p` version.
2. **Operator-side: dual-bundle OTA.**  v2.0.8 OTA payload format
   carries TWO archives:
   ```
   ota-bundle.tar.gz
   ├── manifest.json     (with sha256 of each component)
   ├── Augentix.tar.gz   (our ap2p layer; signed by us)
   └── BSP.rom           (Augentix's image; signed by them)
   ```
   `ap2p` validates our signature on `Augentix.tar.gz`, then POSTs
   `BSP.rom` to the camera's existing `/netsdk/system/operation/upgrade`
   endpoint, which validates Augentix's signature.  Either fails →
   abort, no partial install.
3. **Quarantine + rollback** — if BSP flashes but reboot fails, the
   Augentix bootloader rolls back to the previous BSP slot
   automatically (Augentix already has this in their bootloader on
   most SKUs; needs vendor confirmation per board).
4. **CI integration** — `Augentix.tar.gz` releases on this repo become
   pull-targets for the joint OTA bundle, but we never ship a BSP
   alongside without explicit "approved" stamp from the vendor.

**External dependency:** scope finalization with Augentix.  Treat this as
a coordination task, not a code-only task.  Schedule kickoff with them
right after v2.0.7 ships.

---

## v2.0.9 — full QC against camera feature matrix  *(planning)*

**Goal:** every camera API in the Augentix feature matrix is verified
GREEN against our integrated stack on a real device.

**Source of truth:**
- `印度Adiance-Augentix products group-1.xlsx` sheet `Update` (the
  Augentix-corrected endpoint list, 22 items)
- `Camera_API_Testing_Checklist.docx` (37 items, GET/PUT, tester
  signs off)

**v2.0.9 deliverables:**

1. **`tests/qc/` Python harness.**  One test per checklist row.  Each
   test: PUT/GET via Digest auth admin:(blank), assert HTTP 200,
   assert JSON shape matches expected.  Failures categorized
   (`firmware-too-old`, `auth-broken`, `network-down`, `regression`).
2. **HTML report generator** — `tests/qc/report.html` with pass/fail
   matrix per camera under test.  Includes timestamps + sha256 of
   firmware + raw response JSON for forensics.
3. **Manufacturing fixture** — runbook for QC engineer: connect
   camera, set static IP, run `qc-run.sh <IP>`, read report.  Pass =
   all GREEN, ship the unit.
4. **Per-API expected behavior:**
   - **Working today** (8 items): PromptSoundOem, GSM, image,
     AlarmSound (Siren), AlarmWhiteLight, SwitchTelnet, N1Server
     (HTTP/HTTPS), RTSPServer, RTMPClient — green required.
   - **"Please update firmware" today** (8 items): SearchRecord,
     PlayRecord, ReocrdDownload, RecordDownloadProgress,
     CancelRecordDownload, SearchImage, ImageDownload, HumanDetect,
     Open Telnet — depends on base firmware update (v2.0.8 must land
     first OR vendor must ship updated FW).
   - **Deprecated** (2 items): CustomEventServer GET/PUT — confirm
     replacement endpoint with Augentix, update test.
5. **AlarmSound / AlarmWhiteLight integration with our event pipeline**
   — these endpoints are alarm actuators; v2.0.9 wires them to a
   webhook our cloud can POST to (`/NetAPI/Alarm/AlarmSound` triggered
   by an MQTT case from our broker).

**Acceptance:** 22/22 endpoints GREEN on dev camera with v2.0.5+v2.0.6+
v2.0.7+v2.0.8 stack.  Three additional cameras off the line pass
identically.

---

## v2.1.0 — production + manufacturing grade  *(release criteria)*

**v2.1.0 ships when ALL of the following are true:**

### Functional
- [ ] v2.0.5 fast-boot: avg launcher → live ≤ 8 s, host-observed ≤ 25 s,
      verified on 3 cameras × 5 cycles each.
- [ ] v2.0.6 stream latency: first decoded frame ≤ 500 ms after `SRT up`.
- [ ] v2.0.7 OTA: 10 successful cycles + 1 forced-rollback cycle on dev
      camera; CI-driven.
- [ ] v2.0.8 base FW joint flash: at least 1 BSP+ap2p combined cycle
      verified end-to-end with Augentix sign-off.
- [ ] v2.0.9 QC: 22/22 API checklist items GREEN.

### Reliability / failover / DR
- [ ] `mqttUrl_BKP` failover wired on camera AND backup broker stood up
      in a 2nd GCP region.
- [ ] `CTRL_HOST_BKP`, `EDGE_HOST_BKP`, `RELAY_HOST_BKP` extensions in
      the case-81 retained payload.
- [ ] Kill-broker test passes: take down primary MQTT broker, ≥ 90 %
      of fleet must be live on backup within 60 s.
- [ ] DR runbook (`docs/DR_RUNBOOK.md`) signed off by ops lead.
- [ ] Soak test includes a forced broker swap mid-run; fleet recovers
      without operator action.

### Security
- [ ] All staging credentials rotated:
  - MQTT broker pwd (`Raptor@0`)
  - TURN pwd (`Camera@Secure2024`)
  - Signaling API token (`p2p-server-api-token-change-me`)
  - Dashboard auth (`rtrt`/`rtrt`)
  - HTTP-FLV verify token
- [ ] Per-device telnet password set at manufacturing (no fleet-wide
      shared password).
- [ ] mTLS rolled out (v2.1 secondary objective — see TLS+mTLS
      migration plan): control plane on :443 with per-device cert
      issued by `ArcisAI IoT Intermediate CA HSM`.
- [ ] Brand-leak gate (`branding/verify.sh`) passes on every v2.1.x tag.
- [ ] OTA signing key rotated to production HSM-backed key (not the
      dev key used through v2.0.x).

### Operations
- [ ] `ops/fleet-provision.sh` handles a 2 500-row CSV in < 1 minute.
- [ ] `ops/fleet-rollout.md` playbook signed off by ops lead.
- [ ] Soak test: 1 camera × 7 days continuous run, zero RSS / FD-leak
      growth, log rotation working, OTA-rollback dry-run succeeds.
- [ ] Vendor support contact captured in `docs/SUPPORT.md` (HARD-BLOCK
      release if missing).

### Compliance
- [ ] No secrets in source control (CI gate already enforces).
- [ ] SBOM published for every Augentix.tar.gz release.
- [ ] STQC compliance checklist passed (security/data classification).

### Manufacturing
- [ ] Per-batch CSV format frozen (`serial,verify_token,latency_ms,
      verbose,src_path`) — `ops/fleet-rollout.md` is the contract.
- [ ] First-flash recipe documented + tested on a factory-reset unit.
- [ ] Per-device QC report (HTML) generated automatically as part of
      the manufacturing fixture run.

### Documentation
- [ ] `README.md` v2.1 update with single-paragraph "what this is".
- [ ] `docs/SPACE_BUDGET.md` updated with v2.1 numbers.
- [ ] `docs/SUPPORT.md` with escalation paths.
- [ ] `docs/OPERATIONS.md` runbooks for every alarm + recovery path.

---

## Sequencing + dependencies

```
v2.0.5  → v2.0.6  → v2.0.7  → v2.0.8 ┐
                       │              ├→  v2.0.9  →  v2.1.0
                       │              │
                       └──────────────┘
                       (v2.0.7 enables remote test of v2.0.9
                        QC harness from CI)
```

- v2.0.5: standalone.  Lands first, this week.
- v2.0.6: standalone.  Pair with v2.0.5 verification cycle.
- v2.0.7: depends on v2.0.5 + v2.0.6 (faster boot means more usable
  OTA experiments per day).
- v2.0.8: external dependency on Augentix.  Kickoff right after
  v2.0.7 ships; runs in parallel with QC harness build.
- v2.0.9: needs v2.0.5/.6/.7 in place to be meaningful.  v2.0.8 is
  "nice to have" for v2.0.9 acceptance.
- v2.1.0: integrate everything, harden, document.

## Estimated cadence (calendar, not engineering hours)

| Release | Calendar target | Critical path |
|---|---|---|
| v2.0.5 | Day 0 (today) | CI + user deploy |
| v2.0.6 | Day 0 + 3 | instrument + measure + 1 fix |
| v2.0.7 | Day 0 + 10 | design, ed25519 keygen, 10-cycle test |
| v2.0.8 | Day 0 + 21 | external coordination with Augentix |
| v2.0.9 | Day 0 + 28 | 22 tests × 1 day per test slot |
| **v2.1.0** | **Day 0 + 35** | integration + release dry-run |

Aggressive but achievable if the Augentix coordination unblocks early.
Without it, v2.0.8 slips and v2.1.0 with it.

---

# Roadmap update — post v2.0.7-rc1 (renumbered)

The independent ArcisAI Kitty QC Tool report on `ATPL-200105-ARCIS`
revealed that the MQTT pipeline was the dominant blocker (60.7 % pass
on v2.0.6 due to 18+ "CASE N: Device timeout" rows).  That work was
elevated to **v2.0.7** ahead of the previously-planned TRUE OTA work.
The rest of the roadmap shifts one slot down:

## v2.0.7 — MQTT pipeline fix  *(SHIPPED as v2.0.7-rc1)*

What landed (commit `21f7fbc`):

- **Case 81 ↔ 90 swap**: retained-config moved from `/81` to `/90` so
  case 81 can be tampering-detect GET (matches QC Tool + v1.13.x).
- **case 35** changed from `load_config()` → GET `/netsdk/system/deviceinfo`
  (returns firmwareVersion, which is what QC-024 expects).
- **case 83** NEW: GET `/NetAPI/R.Sync.Stat.NetWork` (network config —
  Augentix Update sheet item 2; was missing, QC-022 timed out).
- **`run_startup_cases()` finally invoked** — `mqtt_thread.c` now spawns
  a detached startup thread after subscribe.  Fires NTP set + TZ set
  (IST GMT+05:30) + deviceInfo + per-channel encoder publishes on
  every boot.  Restores v1.13.x first-init behavior that v2.x had
  silently dropped.
- **Ops scripts** publish to `/90` and clear `/81` (zero-byte retained)
  so stale legacy payloads don't trigger the new tampering handler.

**Acceptance:** user runs `arcisai_kitty_qc_tool.py` against v2.0.7-rc1
on a fresh camera.  Target ≥95 % pass (vs 60.7 % on v2.0.6).  Remaining
fails must be either firmware-gated features (e.g., V2/AI/FaceDetect
"Not available on this firmware") or explicit DEST cases.

## v2.0.8 — TRUE OTA  *(was v2.0.7 in old plan)*

Goal: harden the existing case-36 OTA path so it ships safely without
manual telnet.  Signed-tarball OTA with atomic stage-swap + rollback.

Design unchanged from the original v2.0.7 plan section above — just
the version number changes.

## v2.0.9 — joint BSP + ap2p OTA  *(was v2.0.8)*

External dependency on Augentix.  Coordinate the joint OTA bundle
spec.  Dual-archive payload (our Augentix.tar.gz + their BSP.rom)
with two signatures.

## v2.0.10 — QC harness alignment  *(was v2.0.9)*

Now that we have the actual ArcisAI Kitty QC Tool source
(`hardiksa/CameraUtilityTool`), the "22-item endpoint matrix" can be
replaced with **full alignment to its 64 QC steps**.  Each step gets
a row in the QC report; v2.0.10's acceptance gate is "100 % pass on
the QC Tool minus firmware-gated cases."

Also: the QC tool's feature request from this conversation — **product +
firmware-version combo as initial selector** so the tool loads the
right broker/signaling endpoints (DO-MQTT for some firmware variants,
GCP-MQTT for ours).  Adding that becomes part of v2.0.10's deliverables.

## v2.1.0 — release dry-run + manufacturing

Unchanged in intent.  All criteria from the original v2.1.0 section
above apply.  Plus the new MQTT-related items:

- [ ] v2.0.7 MQTT pipeline 100 % pass on ArcisAI Kitty QC Tool
- [ ] v2.0.8 TRUE OTA 10 cycles + 1 forced rollback on dev camera
- [ ] v2.0.9 joint BSP-OTA verified end-to-end with Augentix sign-off
- [ ] v2.0.10 QC harness aligned, all 64 steps pass on a clean unit
- [ ] Plus all the original v2.1.0 criteria from above

