# Operator runbook — deploying a tagged `kitty-augentix-camera` release to one camera over telnet

**Audience.** An operator pushing a specific tagged release (e.g. `v1.13.2`
or `v2.0.x`) onto a single Augentix HC1703-family camera.  Could be
you-six-months-from-now, a hardware tech, or a partner integrator.

## Three deployment paths — pick the right one

| Path | When to use | Documentation |
|---|---|---|
| **A. Firmware update tool** (`Augentix.tar.gz` recipe) | Factory-flashed camera, or fleet rollout via the standard tool | [`ops/firmware-update-tool.md`](firmware-update-tool.md) |
| **B. v1.13.x → v2.0 cutover** | One existing v1 camera you want to flip to v2 | [`ops/v2.0-cutover-runbook.md`](v2.0-cutover-runbook.md) |
| **C. Manual telnet push of any tagged release** (this doc) | Dev / debug deploys of any tagged release | **continue reading** |

**Scope of THIS guide.** The manual telnet flow — works for both v1.13.x
and v2.0.x.  For production fleet rollouts, prefer **path A**.  For one-shot
v1 → v2 upgrades, prefer **path B**.

**Cross-references.**

* `ops/broker-provisioning-runbook.md` — how to push `provider_srt.conf` from
  the MQTT broker once the new firmware is on the camera.
* `ops/dev-integration-guide.md` — GCP staging endpoints + credentials.
* `SECURITY.md` — credentials cheat sheet (rotation status of every secret
  named below).

---

## Quick reference (1-page TL;DR)

| #  | Step                                                  | Where                |
|----|-------------------------------------------------------|----------------------|
| 1  | `gh release download v<X.Y.Z>` + verify `sha256sum -c` | operator's laptop   |
| 2  | `gcloud compute scp` the three binaries to GCP VM; start `python3 -m http.server 8080` | staging VM `kitty-mqtt-server-del` |
| 3  | Telnet in.  Snapshot md5s, VERSION, `df /mny/mtd/ipc` | target camera        |
| 4  | `killall -9 ambicam.sh` then `killall -9 provider_srt MQTT_vcamclient_Augentix` | target camera |
| 5  | Back up old binaries to `/mnt/tf/kitty-backup-pre-v<X.Y.Z>/` if SD card mounted | target camera |
| 6  | `wget` the three new files into `/tmp/*.new`, verify md5 + size | target camera |
| 7  | `mv /tmp/*.new` over the live binaries, `chmod +x`, bump `VERSION` | target camera |
| 8  | `nohup /mny/mtd/ipc/ambicam.sh </dev/null >/dev/null 2>&1 &` | target camera |
| 9  | Tail `/tmp/ambicam.log` + `provider_srt.log`; from laptop, `mosquitto_sub` for heartbeats | both |
| 10 | Kill the HTTP server on the GCP VM, delete the staged files | staging VM |

All commands below are copy-pasteable as-is — substitute `<X.Y.Z>`,
`<SERIAL>`, and `<CAMERA_IP>` where indicated.

---

## Prerequisites

### Operator's machine

| Tool            | Install                                            | Why                                                       |
|-----------------|----------------------------------------------------|-----------------------------------------------------------|
| `gh` CLI        | `brew install gh` then `gh auth login`             | Pull tagged release tarballs from `Adiance-Technologies`  |
| `expect`        | `brew install expect` / `apt-get install expect`   | Drive the telnet session non-interactively                |
| `mosquitto_pub` / `mosquitto_sub` | `brew install mosquitto` / `apt-get install mosquitto-clients` | Verify the camera publishes heartbeats after restart |
| `gcloud` CLI    | https://cloud.google.com/sdk/docs/install          | scp the binaries to the staging VM + open the HTTP server |
| Network reach   | LAN to camera (TCP/23), public to GCP `:8080`      | Telnet to camera; HTTP fetch from camera to GCP VM        |

`gh` must be authenticated to the **Adiance-Technologies** org and have
`repo` scope.  `gcloud` must have project `arcisai-iot-platform` selected
(`gcloud config set project arcisai-iot-platform`).

### Target camera assumptions

| Item              | Value                                             |
|-------------------|---------------------------------------------------|
| SoC family        | Augentix HC1703 (busybox + uClibc userland)       |
| Telnet            | TCP/23, **enabled** (LAN-only deployments)        |
| Root password     | `adiance@999@arcisai`  (see `SECURITY.md`)        |
| Hostname / SN     | known up front (e.g. `ATPL-200007-TESTA`)         |
| IP address        | known up front                                    |
| Flash partition   | `/mny/mtd/ipc` — 1.5 MB jffs2, typically ~81 % full |
| SD card (if any)  | mounted at `/mnt/tf` — may or may not be present  |

### GCP staging VMs (read-only for the operator)

The camera's NAT typically blocks inbound TCP, so we cannot push files from
the operator's laptop straight into the camera.  Pattern: stage the
binaries on a GCP VM that the camera can `wget` from.

| VM                       | External IP        | Zone             | Purpose                                |
|--------------------------|--------------------|------------------|----------------------------------------|
| `kitty-mqtt-server-del`  | `34.131.134.167`   | `asia-south2-a`  | Temporary HTTP server on :8080         |

Firewall rule `allow-debug-8080` already permits inbound `:8080/tcp` to
instances on the `default` network — no firewall changes needed.

### Critical reminders

* **Production STQC box `34.100.143.36` is OFF LIMITS** — never scp, ssh,
  or otherwise touch it.  Staging only.
* **SD card may not be present.**  Do not depend on `/mnt/tf` being mounted;
  always check `ls /mnt/tf` first, and fall back to a no-backup path with a
  loud warning if it isn't.
* **Flash partition is tight.**  `/mny/mtd/ipc` is 1.5 MB and typically
  ~81 % full.  We download to `/tmp` (tmpfs, ~30 MB free) first, verify,
  then atomically `mv` over the live file — never overlap old + new on
  flash.
* **No `pkill`.**  Busybox has `kill`, `killall`, `pidof` only.

---

## Step 1 — Pull the release locally

```sh
mkdir -p /tmp/kitty-v<X.Y.Z>
gh release download v<X.Y.Z> \
    --repo Adiance-Technologies/kitty-augentix-camera \
    --dir  /tmp/kitty-v<X.Y.Z>
ls -la /tmp/kitty-v<X.Y.Z>
```

Expected listing:

```
kitty-augentix-camera-<X.Y.Z>.tar.gz
kitty-augentix-camera-<X.Y.Z>.tar.gz.sha256
```

Extract + verify:

```sh
cd /tmp/kitty-v<X.Y.Z>
sha256sum -c kitty-augentix-camera-<X.Y.Z>.tar.gz.sha256
tar -xzf kitty-augentix-camera-<X.Y.Z>.tar.gz
ls -la ambicam/
```

`ambicam/` will contain (per `.github/workflows/build-augentix.yml`):

```
ambicam.sh
provider_srt
MQTT_vcamclient_Augentix
provider_srt.conf.example
kitty.logrotate
VERSION
BUILT_AT
COMMIT
```

Spot-check integrity:

```sh
file ambicam/provider_srt ambicam/MQTT_vcamclient_Augentix ambicam/ambicam.sh
```

Expected:

```
ambicam/provider_srt:              ELF 32-bit LSB executable, ARM, EABI5, dynamically linked, ...
ambicam/MQTT_vcamclient_Augentix:  ELF 32-bit LSB executable, ARM, EABI5, dynamically linked, ...
ambicam/ambicam.sh:                Bourne-Again shell script, ASCII text executable
```

If `provider_srt` or `MQTT_vcamclient_Augentix` come back as anything other
than **ARM ELF** — stop.  You're holding the wrong artifact.

Record the local md5s — you'll compare against the on-camera copies later:

```sh
md5sum ambicam/provider_srt ambicam/MQTT_vcamclient_Augentix ambicam/ambicam.sh
```

---

## Step 2 — Stage the release on a GCP VM

scp the three deployable files to the staging VM:

```sh
gcloud compute scp \
    /tmp/kitty-v<X.Y.Z>/ambicam/provider_srt \
    /tmp/kitty-v<X.Y.Z>/ambicam/MQTT_vcamclient_Augentix \
    /tmp/kitty-v<X.Y.Z>/ambicam/ambicam.sh \
    kitty-mqtt-server-del:/tmp/ \
    --zone=asia-south2-a
```

Start a temporary HTTP server on the VM, backgrounded so the SSH session
exits cleanly:

```sh
gcloud compute ssh kitty-mqtt-server-del --zone=asia-south2-a --command="\
    setsid python3 -m http.server 8080 --bind 0.0.0.0 --directory /tmp \
        </dev/null >/tmp/http.log 2>&1 & disown; \
    sleep 2; ss -tln | grep 8080"
```

Expected last line:

```
LISTEN  0  5  0.0.0.0:8080  0.0.0.0:*
```

From the operator's laptop, confirm external reachability:

```sh
curl -s -I --max-time 5 http://34.131.134.167:8080/provider_srt | head -3
```

Expected:

```
HTTP/1.0 200 OK
Server: SimpleHTTP/0.6 Python/3.x
Content-type: application/octet-stream
```

If you get a connection timeout — the firewall rule was removed or the
HTTP server didn't start.  Re-run the `setsid` line and check
`/tmp/http.log` on the VM.

---

## Step 3 — Pre-flight on the target camera

All telnet steps drive through `expect`.  Save the following as
`/tmp/camera-preflight.exp`, replacing `<CAMERA_IP>`:

```expect
#!/usr/bin/expect -f
set timeout 30
log_user 1
spawn telnet <CAMERA_IP>
expect "login:"; send "root\r"
expect "assword:"; send "adiance@999@arcisai\r"
expect "# "
send "PS1='zq> '\r"
expect "zq> "

send "echo MARK-version && cat /mny/mtd/ipc/ambicam/VERSION 2>/dev/null && echo MARK-version-DONE\r"
expect "MARK-version-DONE"; expect "zq> "

send "echo MARK-md5 && md5sum /mny/mtd/ipc/ambicam.sh /mny/mtd/ipc/ambicam/provider_srt /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix && echo MARK-md5-DONE\r"
expect "MARK-md5-DONE"; expect "zq> "

send "echo MARK-df && df -h /mny/mtd/ipc /tmp /mnt/tf 2>&1 && echo MARK-df-DONE\r"
expect "MARK-df-DONE"; expect "zq> "

send "echo MARK-ipc-ls && ls -la /mny/mtd/ipc/ambicam/ && echo MARK-ipc-ls-DONE\r"
expect "MARK-ipc-ls-DONE"; expect "zq> "

send "echo MARK-ps && ps w | grep -E 'ambicam|provider_srt|MQTT_vcam' | grep -v grep && echo MARK-ps-DONE\r"
expect "MARK-ps-DONE"; expect "zq> "

send "echo MARK-tools && which wget curl ftpget 2>/dev/null && echo MARK-tools-DONE\r"
expect "MARK-tools-DONE"; expect "zq> "

send "exit\r"; expect eof
```

Run it: `chmod +x /tmp/camera-preflight.exp && /tmp/camera-preflight.exp`.

Read the output carefully and confirm:

| Check                                | Expected                                                                |
|--------------------------------------|-------------------------------------------------------------------------|
| `VERSION` file                       | A version string (`1.13.0`, `1.13.1`, etc.) — record the **from**-version |
| md5 of binaries                      | Distinct from the new release md5s you recorded in Step 1               |
| `df /mny/mtd/ipc`                    | Available column **≥ 700 KB** before swap                               |
| `df /tmp`                            | Available **≥ 5 MB** (it's tmpfs — plenty in practice)                  |
| `df /mnt/tf`                         | Either lists a real SD card, or returns an error (no SD)                |
| `ps` output                          | Records the **current** PIDs for `ambicam.sh`, `provider_srt`, `MQTT_vcamclient_Augentix` |
| `wget` available                     | `which wget` returns a path (it's busybox-builtin)                      |

Sample healthy output (from a real v1.13.1 → v1.13.2 deploy on
`192.168.12.129`):

```
zq> cat /mny/mtd/ipc/ambicam/VERSION
1.13.1
zq> md5sum /mny/mtd/ipc/ambicam.sh /mny/mtd/ipc/ambicam/provider_srt /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix
e3c4...  /mny/mtd/ipc/ambicam.sh
9f81...  /mny/mtd/ipc/ambicam/provider_srt
72d2...  /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix
zq> df -h /mny/mtd/ipc /tmp
Filesystem      Size      Used Available Use%  Mounted on
/dev/mtdblock7  1.5M    1.2M    276.0K   81%   /mny/mtd/ipc
tmpfs          29.9M  104.0K     29.8M    0%   /tmp
```

**Headroom check.**  If `df /mny/mtd/ipc` shows < 700 KB available
(because logs grew or a stale backup lives on flash), free space **before**
proceeding:

```sh
# In the same telnet session — only run what's relevant:

# A. Stale truncated stub from a failed prior backup attempt: delete it.
ls -la /mny/mtd/ipc/ambicam/provider_srt.pre-v1.13.0 2>/dev/null
rm -f /mny/mtd/ipc/ambicam/provider_srt.pre-v1.13.0

# B. Big logs: truncate, don't delete (preserve the inode so live FDs survive).
: > /mny/mtd/ipc/ambicam/MQTT_vcamclient.log
: > /mny/mtd/ipc/ambicam/provider_srt.log

df -h /mny/mtd/ipc
```

> The `.pre-v1.13.0` file is a known artefact: a v1.13.0 deploy attempt
> created it but the copy was truncated mid-write.  It's pure dead weight —
> deleting it gives back ~70 KB.

---

## Step 4 — Stop the running daemons

`ambicam.sh` is the launcher: it respawns the two daemons on a 10-second
monitor loop, so we **must** kill it first.  Then kill the daemons.

```sh
killall -9 ambicam.sh
sleep 1
killall -9 provider_srt MQTT_vcamclient_Augentix
sleep 2
ps w | grep -E 'ambicam|provider_srt|MQTT_vcam' | grep -v grep
```

Expected: the final `ps` line returns **no output** (or just the grep
itself, which we already filter).  If you still see a daemon — somebody
launched it from a place we don't know about; investigate before
proceeding.

> **Reminder.**  Busybox lacks `pkill`.  Use `killall` (matches by command
> name) and `kill -9 <pid>` (matches by pid from `pidof`).  `pkill -f` is
> a bashism that will simply not find a binary on this device.

---

## Step 5 — Backup the existing binaries

If `/mnt/tf` is mounted (`df /mnt/tf` returned a real filesystem in
Step 3), take a backup.  The SD card is the only on-camera storage with
room for a full set.

```sh
mkdir -p /mnt/tf/kitty-backup-pre-v<X.Y.Z>
cp /mny/mtd/ipc/ambicam.sh                                  /mnt/tf/kitty-backup-pre-v<X.Y.Z>/
cp /mny/mtd/ipc/ambicam/provider_srt                        /mnt/tf/kitty-backup-pre-v<X.Y.Z>/
cp /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix            /mnt/tf/kitty-backup-pre-v<X.Y.Z>/
cp /mny/mtd/ipc/ambicam/config.json                         /mnt/tf/kitty-backup-pre-v<X.Y.Z>/
cp /mny/mtd/ipc/ambicam/VERSION                             /mnt/tf/kitty-backup-pre-v<X.Y.Z>/
ls -la /mnt/tf/kitty-backup-pre-v<X.Y.Z>/
md5sum /mnt/tf/kitty-backup-pre-v<X.Y.Z>/*
```

Confirm the md5s in the backup match the **from**-version md5s you
recorded in Step 3.

If `/mnt/tf` is **not** mounted:

```sh
# No SD card — proceed without on-camera backup.
echo "WARNING: /mnt/tf not mounted; deploying without on-camera backup."
echo "         If something goes wrong, you'll need physical access to recover."
```

Continue, but log this loudly and recommend the next operator insert an SD
card before the next deploy.

---

## Step 6 — `wget` the new binaries onto the camera

We download to `/tmp` (tmpfs, ~30 MB free) so the old + new copies never
overlap on the 1.5 MB flash partition.

```sh
cd /tmp
wget -q http://34.131.134.167:8080/provider_srt             -O /tmp/provider_srt.new
wget -q http://34.131.134.167:8080/MQTT_vcamclient_Augentix -O /tmp/MQTT_vcamclient_Augentix.new
wget -q http://34.131.134.167:8080/ambicam.sh               -O /tmp/ambicam.sh.new
ls -la /tmp/*.new
md5sum /tmp/*.new
```

Expected (size + md5 must both match the local copies recorded in Step 1):

```
-rw-r--r--    1 root  root  616k  May 18 12:34 /tmp/MQTT_vcamclient_Augentix.new
-rw-r--r--    1 root  root  741k  May 18 12:34 /tmp/provider_srt.new
-rwxr-xr-x    1 root  root   18k  May 18 12:34 /tmp/ambicam.sh.new
9a3f...  /tmp/MQTT_vcamclient_Augentix.new
7b2e...  /tmp/provider_srt.new
4c11...  /tmp/ambicam.sh.new
```

If any md5 differs from the laptop md5 (Step 1): the transfer truncated.
Delete the `.new` file and retry just that one `wget`.  Repeated truncation
points at the camera's outbound HTTP being interfered with; see
**Troubleshooting** below.

---

## Step 7 — Install (atomic moves)

```sh
mv /tmp/provider_srt.new             /mny/mtd/ipc/ambicam/provider_srt
chmod +x                              /mny/mtd/ipc/ambicam/provider_srt

mv /tmp/MQTT_vcamclient_Augentix.new /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix
chmod +x                              /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix

mv /tmp/ambicam.sh.new               /mny/mtd/ipc/ambicam.sh
chmod +x                              /mny/mtd/ipc/ambicam.sh

echo <X.Y.Z> > /mny/mtd/ipc/ambicam/VERSION

md5sum /mny/mtd/ipc/ambicam.sh /mny/mtd/ipc/ambicam/provider_srt /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix
cat /mny/mtd/ipc/ambicam/VERSION
df -h /mny/mtd/ipc
```

Expected: the three md5s **now match the laptop md5s from Step 1**, the
`VERSION` file reads `<X.Y.Z>`, and `df` still shows non-trivial headroom
(if it doesn't, truncate logs — see Step 3 headroom check).

> `mv` within the same filesystem is atomic — the old inode is replaced by
> the new one in one rename(2).  This is why we download to `/tmp` only as
> a staging step; the final move into `/mny/mtd/ipc/ambicam/` is a
> cross-filesystem `mv`, which is **not** atomic.  That's why we kill the
> daemons in Step 4 first: nothing is reading the old inode at the moment
> of replacement.

---

## Step 8 — Restart the launcher

```sh
nohup /mny/mtd/ipc/ambicam.sh </dev/null > /dev/null 2>&1 &
sleep 6
ps w | grep -E 'ambicam|provider_srt|MQTT_vcam' | grep -v grep
```

Expected: **three** processes — `ambicam.sh`, `provider_srt`,
`MQTT_vcamclient_Augentix`.  Sample (from a clean v1.13.2 restart):

```
zq> ps w | grep -E 'ambicam|provider_srt|MQTT_vcam' | grep -v grep
  493 root      4084 S  /bin/sh /mny/mtd/ipc/ambicam.sh
  502 root     12 MB S  /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix
  519 root     14 MB S  /mny/mtd/ipc/ambicam/provider_srt
```

If `provider_srt` is missing — the launcher's monitor loop will respawn it
within 10 seconds.  Wait, re-check.  Still missing after 30 seconds → see
**Troubleshooting** below.

---

## Step 9 — Verify

### 9a. Launcher log

```sh
tail -20 /tmp/ambicam.log
```

Expected lines, in order:

```
2026-05-18 12:34:56 - ambicam.sh starting
2026-05-18 12:34:58 - filesystem ready (/mny/mtd/ipc/ambicam)
2026-05-18 12:34:58 - creating library symlinks...
2026-05-18 12:34:58 - internet probe: mqtt-staging.devices.arcisai.io:443
2026-05-18 12:34:59 - starting MQTT_vcamclient_Augentix
2026-05-18 12:35:00 - starting provider_srt
2026-05-18 12:35:00 - all available services launched
```

### 9b. provider_srt log

```sh
tail -10 /mny/mtd/ipc/ambicam/provider_srt.log
```

Expected:

```
provider_srt v3 ... service_id=<SERIAL>, signaling=signaling.devices.arcisai.io:80
signaling: registered as <SERIAL>
```

If signaling host shows the old `*.do.arcisai.io` host: the broker still
holds a stale `provider_srt.conf`.  See `ops/broker-provisioning-runbook.md`
for the case-81 push procedure.

### 9c. MQTT heartbeats — from the operator's laptop

```sh
mosquitto_sub \
    -h mqtt-staging.devices.arcisai.io -p 443 \
    -u Torque -P 'Raptor@0' \
    -t "torque/tx/<SERIAL>/#" -v
```

Within 10 seconds you should see heartbeat + config publishes from the
camera.  Sample:

```
torque/tx/<SERIAL>/heartbeat {"ts":1747...,"uptime":12,"ver":"<X.Y.Z>"}
torque/tx/<SERIAL>/config    {"signaling":"signaling.devices.arcisai.io:80",...}
```

Hit `Ctrl-C` once you've seen at least one heartbeat + one config publish.
(For full endpoint + credential details, see
`ops/dev-integration-guide.md`.)

---

## Step 10 — Teardown the GCP HTTP server

```sh
gcloud compute ssh kitty-mqtt-server-del --zone=asia-south2-a --command="\
    pkill -f 'python3 -m http.server' ; \
    rm -f /tmp/provider_srt /tmp/MQTT_vcamclient_Augentix /tmp/ambicam.sh /tmp/http.log"
```

Verify externally:

```sh
curl -s -o /dev/null -w '%{http_code}\n' --max-time 3 http://34.131.134.167:8080/
```

Expected: `000` (connection refused — the server is dead).  Any other
code means the HTTP server is still running and serving binaries — kill
it before walking away.

---

## Rollback procedure

Use this if the new daemons crash-loop, the camera stops publishing
heartbeats for more than 60 seconds, or anything else goes visibly wrong.

Requires the `/mnt/tf/kitty-backup-pre-v<X.Y.Z>/` directory from Step 5.
If you skipped Step 5 (no SD card), you're not rolling back over telnet —
recover via physical access.

```sh
# In the camera telnet session:

# 1. Stop the misbehaving daemons (launcher first).
killall -9 ambicam.sh
sleep 1
killall -9 provider_srt MQTT_vcamclient_Augentix
sleep 2

# 2. Restore the previous binaries.
cp /mnt/tf/kitty-backup-pre-v<X.Y.Z>/ambicam.sh                /mny/mtd/ipc/ambicam.sh
cp /mnt/tf/kitty-backup-pre-v<X.Y.Z>/provider_srt              /mny/mtd/ipc/ambicam/provider_srt
cp /mnt/tf/kitty-backup-pre-v<X.Y.Z>/MQTT_vcamclient_Augentix  /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix
cp /mnt/tf/kitty-backup-pre-v<X.Y.Z>/VERSION                   /mny/mtd/ipc/ambicam/VERSION

chmod +x /mny/mtd/ipc/ambicam.sh
chmod +x /mny/mtd/ipc/ambicam/provider_srt
chmod +x /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix

# 3. Verify md5s match what you recorded in Step 3 (the pre-deploy state).
md5sum /mny/mtd/ipc/ambicam.sh /mny/mtd/ipc/ambicam/provider_srt /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix
cat /mny/mtd/ipc/ambicam/VERSION

# 4. Restart launcher.
nohup /mny/mtd/ipc/ambicam.sh </dev/null > /dev/null 2>&1 &
sleep 6
ps w | grep -E 'ambicam|provider_srt|MQTT_vcam' | grep -v grep

# 5. Re-run Step 9 verifications.
```

After rollback, **don't immediately retry the deploy** — capture
`/tmp/ambicam.log` and `provider_srt.log` from the failed attempt
(they're tmpfs, so they survive only until reboot), file an issue with
the logs attached, and triage before another attempt.

---

## Troubleshooting common failures

### Camera can't reach `34.131.134.167:8080`

Symptom: `wget` in Step 6 hangs or returns connection-refused / network
unreachable.

Diagnosis:

```sh
# In the camera telnet session:
ping -c 3 -W 3 34.131.134.167
nc -w 5 -z 34.131.134.167 8080 && echo NC-OK || echo NC-FAIL
nslookup mqtt-staging.devices.arcisai.io
```

If ping/`nc` fail: the camera's network blocks outbound to GCP (uplink
firewall, captive portal, no default route).  Two alternatives:

* **Same-LAN fallback.**  If your operator laptop is on the same LAN as
  the camera, you can `python3 -m http.server 8765` on the laptop and
  point `wget` at `http://<laptop-LAN-IP>:8765/...`.
* **Avoid `nc` for binary transfer.**  We hit this at v1.13.0: `nc -w`
  fires a timeout that truncates the binary mid-stream, producing a
  corrupt copy that md5-fails or — worse — runs partway and crashes.
  Stick with `wget`.

### md5 mismatch after `wget`

Connection truncated mid-fetch.  Symptoms: the `.new` file is shorter
than expected, md5 doesn't match.

Recovery:

```sh
rm -f /tmp/<file>.new
# wget retry — no special flag needed; wget streams the full content or fails clearly.
wget -q http://34.131.134.167:8080/<file> -O /tmp/<file>.new
ls -la /tmp/<file>.new
md5sum /tmp/<file>.new
```

If it truncates twice in a row, the upstream is dropping the connection —
check `/tmp/http.log` on the GCP VM and `dmesg` on the camera for clues.

### Camera flash > 90 % after install

Symptom: `ambicam.sh` fires the `flash_full` alarm (see `deploy/ambicam.sh:114`)
and starts truncating logs on its own — but if it's full it can't run
correctly.

Recovery:

```sh
df -h /mny/mtd/ipc
ls -laS /mny/mtd/ipc/ambicam/    # sorted by size, biggest first
: > /mny/mtd/ipc/ambicam/MQTT_vcamclient.log
: > /mny/mtd/ipc/ambicam/provider_srt.log
rm -f /mny/mtd/ipc/ambicam/provider_srt.pre-v1.13.0 2>/dev/null
df -h /mny/mtd/ipc
```

If still over 90 %, you have stale `.gz` rotations on flash — remove the
oldest ones (`/mny/mtd/ipc/ambicam/*.log.3.gz`, then `.2.gz`).

### Camera doesn't respawn `provider_srt` after MQTT case-81 push

This is the **busybox-pkill bug** that v1.13.1 shipped with and v1.13.2
fixed.  Symptom: broker pushes a new `provider_srt.conf`, the sentinel
`/tmp/provider_srt_conf_pushed` appears, but `provider_srt` is never
restarted to pick it up.

Diagnosis:

```sh
cat /mny/mtd/ipc/ambicam/VERSION
```

If the version is `1.13.1` or earlier — that's the bug.  Deploy `v1.13.2`
or newer (i.e. **re-run this whole guide** with `<X.Y.Z>` ≥ `1.13.2`).

If the version is `1.13.2` or newer and the respawn still doesn't fire —
this is a new bug; capture `/tmp/ambicam.log` and file an issue.

### Telnet login times out

Symptom: `expect` script hangs at `login:` for 30 seconds, then exits.

Likely causes:

* The camera isn't actually reachable.  Try `ping <CAMERA_IP>` from the
  laptop.
* Telnet was disabled on this device.  Telnet is LAN-only and is
  **disabled-by-default on most production deployments** (see
  `SECURITY.md`).  You may be looking at a hardened unit that needs
  physical access or a manufacturing-line tool.
* The credentials changed.  The `adiance@999@arcisai` root password is
  the manufacturing default; some units have per-device rotated passwords
  recorded in the device asset tracker.

---

## Final pointer — v2.0

For the v2.0 cutover (single `ap2p` binary replacing
`provider_srt` + `MQTT_vcamclient_Augentix`, brand-neutral
`/mny/mtd/ipc/ap2p/` layout, mTLS signaling on `:443`) see
`ops/v2.0-cutover-runbook.md` once it's published.  The v2.0 deployment
pattern differs significantly — **do not** reuse this guide for v2.x.
