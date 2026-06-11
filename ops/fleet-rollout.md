# Fleet rollout — provisioning N cameras with v2.x ap2p

How to bring a batch of 2 500 (or 25 or 25 000) Augentix HC1703 cameras
into production with per-device config, end-to-end.

The architecture is the same as it has been since v1.13 — **broker holds
each device's config; cameras pull on subscribe** — only the file format
on disk and the wire-protocol field names changed.  Both the firmware
flash and the config provisioning are mass-publishable: nothing in the
hot path is per-device manual work.

---

## Two independent provisioning channels

| Channel | Per-device data flowing through | When |
|---|---|---|
| **A. Firmware-update tool** (`Augentix.tar.gz`) | `ap2p`, `ambicam.sh`, `config.json` (broker URL + creds), `BurnUID` (already on flash from manufacturing) | Once per camera at first-flash / firmware-upgrade.  Same operator recipe for every camera in the batch. |
| **B. MQTT-retained config publisher** (`ops/fleet-provision.sh`) | NODE_ID, CTRL_*, EDGE_*, RELAY_*, SRC_PATH (`?verify=` is per-device), LATENCY_MS, VERBOSE | Once per camera, from operator's laptop / jumpbox.  Broker stores the retained payload forever; cameras receive on every subscribe. |

Channels A and B are independent.  Order doesn't matter — if A runs first,
the camera waits in `mqtt_thread_main` until B publishes its retained
payload.  If B runs first, the broker holds the payload until the camera
finishes A and subscribes.

---

## Step-by-step for a 2 500-unit batch

### Step 1 — Manufacturing assembly captures per-device data

For each camera coming off the line:

| Field | Source |
|---|---|
| `serial` (= NODE_ID = BurnUID) | Burned into `/mny/mtd/ipc/BurnUID` at manufacturing.  Format `ATPL-NNNNNN-XXXXX`. |
| `verify_token` | Per-device random token; written into manufacturing record AND will go into MQTT retained payload's `SRC_PATH=?verify=…`. |
| (Optional) `latency_ms`, `verbose`, `src_path` overrides | Per-camera tuning. |

Manufacturing exports a single CSV per batch:

```csv
serial,verify_token,latency_ms,verbose,src_path
ATPL-200001-TESTA,aF1Z6kQpRNTzv8Xh,300,0,/flv/live_ch0_0.flv?verify=aF1Z6kQpRNTzv8Xh
ATPL-200002-TESTA,bG2A7lRqSOUaw9Yi,300,0,/flv/live_ch0_0.flv?verify=bG2A7lRqSOUaw9Yi
... (2 500 rows) ...
```

Required column: `serial`.  Others have defaults — see `ops/fleet-provision.sh --help`.

### Step 2 — Flash the firmware (Channel A)

Run the standard firmware-update tool's recipe (`ops/firmware-update-tool.md`):

```
wget Augentix.tar.gz                                # one URL, same for every device
./tar -xvf → AugenTix/; chmod 777 *
mkdir /etc/jffs2/ambicam /mny/mtd/ipc/ambicam
cp *.crt client.key /etc/jffs2/ambicam               # placeholder for KITTY_NO_TLS
cp *                /mny/mtd/ipc/ambicam
rm /mny/mtd/ipc/ambicam/{*.crt,client.key}
mv ambicam.sh ../.
reboot
```

This is run **as-is on every camera** — no per-device customisation in
this step.  The Augentix.tar.gz on `prong.arcisai.io` is identical for
every unit in the batch.  Camera comes back up, reads `config.json` for
MQTT bootstrap, connects to broker, subscribes to `torque/rx/<BurnUID>/#`
…and then waits for Channel B.

### Step 3 — Publish per-device retained MQTT payloads (Channel B)

On the operator's laptop / jumpbox:

```sh
export BROKER_PASS='<prod-broker-password>'
export CTRL_TOKEN='<prod-signaling-api-token>'
export RELAY_PASS='<prod-turn-password>'

# Recommended dry-run first (no broker writes):
ops/fleet-provision.sh batch-2026-05-21.csv --dry-run

# Real run:
ops/fleet-provision.sh batch-2026-05-21.csv
```

Output looks like:
```
fleet-provision: broker=mqtt.devices.arcisai.io:443  ctrl=signaling.devices.arcisai.io:443  csv=batch-2026-05-21.csv  dry_run=0
  progress: 100 rows published
  progress: 200 rows published
  ...
  progress: 2500 rows published
fleet-provision: rows=2500  published=2500  skipped=0  failed=0
```

Each `mosquitto_pub --retain` takes ~10 ms; 2 500 publishes run in well
under a minute.  Broker memory after the run holds 2 500 retained
payloads on 2 500 distinct topics — about 1 MB of broker RAM total.

### Step 4 — Verification

For one representative camera per batch (or all, if you're feeling
thorough):

```sh
# Subscribe to its tx/ stream — heartbeats + status appear within ~10 s
mosquitto_sub -h $BROKER_HOST -p $BROKER_PORT \
              -u $BROKER_USER -P "$BROKER_PASS" \
              -t "torque/tx/<serial>/#" -v -W 30
```

You should see config publishes back from the camera (cases 1, 2, 6, 38,
39).  Camera is healthy + on-line.

To verify control-plane registration centrally, query the signaling
server's dashboard at `signaling.devices.arcisai.io` (admin auth).

---

## Config rotation post-deploy

To change any field (TURN password rotated, signaling host moved, etc.):

1. Edit the relevant column in your batch CSV (or regenerate it).
2. Re-run `ops/fleet-provision.sh <new-csv>`.
3. Done.  Every connected camera picks up the new payload within seconds
   (case-81 hot reload); reboots also pick it up automatically because
   the broker retained the latest.

To decommission a single camera:
```sh
mosquitto_pub -h $BROKER_HOST -p $BROKER_PORT \
              -u $BROKER_USER -P "$BROKER_PASS" \
              -t "torque/rx/<serial>/81" \
              --retain -n            # zero-byte retained payload clears it
```

The camera will receive an empty payload on next subscribe, fail to parse
it (CTRL_HOST empty), and stay in the "waiting for valid config" state
indefinitely — effectively offline.  Reverse by publishing real config
again.

---

## Operational concerns

### How much config is per-device, really?

| Field | Per-device? | Why |
|---|---|---|
| NODE_ID | YES — comes from BurnUID, no broker action needed | Manufacturing-set serial |
| SRC_PATH `?verify=<token>` | YES — embed per-device verify token in the URL | Per-device HTTP-FLV auth |
| Everything else (CTRL_HOST, RELAY_USER, etc.) | NO — fleet-wide | Shared infrastructure |

So the per-device row in your CSV is really just `(serial, verify_token)`
plus optional tuning knobs.  Two columns.

### What happens if a camera misses its retained payload?

It can't.  MQTT retained semantics guarantee:
- If the camera is currently online when the operator publishes, it
  receives the payload immediately (broker forwards normally).
- If the camera comes online *after* the operator publishes, the broker
  delivers the most recent retained payload on subscribe.
- If the camera reboots, same as above — re-subscribes, broker
  re-delivers.

The only failure mode is if the operator publishes BEFORE the camera's
`NODE_ID` is correct (e.g., during a topic-name typo).  Spot-check the
CSV by running `--dry-run` first.

### Will `ap2p.log` fill the camera's flash?

No — v2.0.x `ambicam.sh` has a background log-size watchdog (256 KB cap,
copy-truncate, keeps one prior rotation as `ap2p.log.old`).  At the
typical 60-byte/sec log rate, rotations happen ~hourly.  Flash usage
on `/mny/mtd/ipc` stays bounded.

If you set `VERBOSE=1` in a camera's retained payload, the log rate
goes up but the rotation cadence keeps it bounded.  For 2 500-unit
production rollout, recommend `VERBOSE=0` per camera (set the verbose
column in CSV to 0); switch to `1` only when actively debugging a
specific unit.

### Bandwidth and broker load

- 2 500 cameras × 1 MQTT connection each → 2 500 long-lived MQTT
  sessions.  Mosquitto handles this comfortably on a small VM (~50 MB
  RAM for the connection table).
- Each camera publishes ~5 small JSON messages every ~30 s as
  heartbeat / config-echo.  Total broker ingress: ~400 msg/s, ~30 KB/s.
- Each camera receives only its own retained payload on subscribe
  (one-shot, ~500 bytes) plus any operator pushes.  Broker egress:
  baseline near zero except during fleet rotations.

The bottleneck is the signaling server (one TCP connection per camera)
and the SRT data path (one UDP tunnel per active viewer).  Neither is
correlated with batch size in steady state — both scale with the number
of *active* (viewer-connected) cameras at any moment.

---

## Pre-deploy checklist

Before running `ops/fleet-provision.sh` on a fresh batch:

- [ ] Manufacturing has exported the per-batch CSV with `serial,verify_token,…` rows for every unit.
- [ ] Manufacturing has flashed `Augentix.tar.gz` v2.x.y on every unit (Channel A).
- [ ] `BROKER_PASS`, `CTRL_TOKEN`, `RELAY_PASS` env vars are set on the operator host (not committed anywhere).
- [ ] `ops/fleet-provision.sh <csv> --dry-run` printed every expected serial without errors.
- [ ] One representative camera from the batch is online and `mosquitto_sub -t 'torque/tx/<that-serial>/#'` is streaming heartbeats.

Once green: run the real `ops/fleet-provision.sh`, watch the progress
markers, verify by sampling 10–20 cameras' `torque/tx/<serial>/#` topics.
