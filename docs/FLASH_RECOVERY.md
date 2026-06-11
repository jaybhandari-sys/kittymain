# Flash-kitty Recovery Guide

This doc covers what to do when a `flash-kitty.py` run fails or leaves a
camera in an unexpected state.  It assumes a v2.0.x Augentix HC1703
camera with `/mny/mtd/ipc` (1.5 MB JFFS2 partition).

## TL;DR — Common Failure Modes

| Symptom                                          | Fix                                |
|--------------------------------------------------|------------------------------------|
| `___RECIPE_FAILED_GC___` (JFFS2 GC didn't free)  | `reboot`, then re-run flash-kitty  |
| `___RECIPE_FAILED_INSTALL___` (cp ENOSPC mid-recipe) | `reboot`, then re-run flash-kitty |
| `cp: No space left on device` (legacy v1 flash)  | upgrade to flash-kitty v3 (hardened) |
| Camera ping OK but ports 80/23 BLOCKED           | factory-reset, then re-flash       |
| `BurnUID` empty, ap2p crash-loops "no NODE_ID"   | `echo 'ATPL-NNNNNN-XXXXX' > /mny/mtd/ipc/BurnUID; sync; reboot` |
| view-feed-fast.sh "Invalid data found"           | verify fresh boot publish first    |

## What the hardened flash-kitty does to prevent bricking

The v3 hardened recipe (`tools/flash-kitty.py`) defends against the
JFFS2 ENOSPC-mid-install class of bug by:

1. **Validate tarball entirely in RAM** before touching `/mny/mtd/ipc`.
   If the tarball is corrupt or missing files, we never reach the rm.
2. **Preserve `BurnUID`** to `/tmp/BurnUID.preserved` BEFORE any rm.
3. **Stop ap2p + ambicam.sh** so the watchdog won't relaunch ap2p
   mid-install with stale binaries.
4. **rm old install, then `sync; sync; sync`** to flush kernel caches.
5. **Active GC wait loop** (up to 30 iterations, ≈30 s): each iteration
   writes a 4 KB sentinel file, syncs, removes it, syncs.  Each write
   nudges JFFS2's GC walker to actually erase stale blocks.  Loop exits
   when `df` reports enough free space for the install + 64 KB headroom.
6. **Install in size-descending order**: `ap2p` (667 KB) first, then
   `libcurl.so.4` (425 KB), then `libpaho-mqtt3cs.so.1` (110 KB), then
   tiny files.  If we hit ENOSPC partway, the big files are already on
   disk so the camera is at least bootable.
7. **Per-file space verification**: after each `cp`, check `df` free
   space.  If `cp` failed OR free dropped to a danger threshold, ABORT.
8. **Restore `BurnUID`** from snapshot.
9. **Post-install verification**: all required files must exist.  Recipe
   succeeds only if every `test -f` passes.
10. **Failure path**: on any abort, restore BurnUID and leave the camera
    in a deterministic "ap2p stopped, ambicam removed" state.  The
    vendor's HTTP / RTSP services keep running independently, so the
    camera remains reachable and re-flashable.

## Recovery procedures by failure mode

### A. `___RECIPE_FAILED_GC___` — JFFS2 GC couldn't free enough space

What it means: the JFFS2 partition is heavily fragmented (common after a
factory reset or a previous failed flash).  The kernel's GC didn't catch
up within 30 s.

What's safe: the camera is **NOT bricked**.  ap2p is stopped, ambicam/
is removed, BurnUID is preserved.  HTTP and RTSP still work.

Fix:

```bash
telnet 192.168.12.129
> root / adiance@999@arcisai
# reboot
```

The reboot triggers a clean JFFS2 mount-time scan which fully reclaims
the partition.  Then:

```bash
tools/flash-kitty.py --ip 192.168.12.129 --tag v2.0.7-rc2
```

### B. `___RECIPE_FAILED_INSTALL___` — cp failed mid-install

Same root cause class as A.  Same fix.  The `INSTALL_FAIL: cp <name>`
line above the `___RECIPE_FAILED_INSTALL___` marker tells you exactly
which file failed.

### C. Camera ping OK but ports 80/23 BLOCKED

This means the camera's main application (which serves HTTP + accepts
telnet) crashed or never started.  Most likely cause: `BurnUID` got
wiped during a previous failed flash, so ap2p crash-loops with
"no NODE_ID".

Fix (vendor's HW button):

1. Hold the reset button on the camera for 10 s (factory reset).
2. Wait 60 s for it to fully boot.
3. Verify ports come back: `nc -z -w 2 <ip> 80 && nc -z -w 2 <ip> 23`
4. Re-flash: `tools/flash-kitty.py --ip <ip> --tag v2.0.7-rc2`

Fix (if you can still reach NetSDK over port 80 but not telnet):

```bash
# Enable telnet via HTTP PUT
curl -u admin: -X PUT http://<ip>/NetAPI/R.SwitchTelnet -d '{"Enable":true}'
# Wait ~10 s for telnet to come up, then telnet in and inspect
```

### D. `BurnUID` empty — ap2p crash-loops "no NODE_ID"

After a flash that wiped BurnUID, ap2p logs (in `/mny/mtd/ipc/ambicam/ap2p.log`):

```
[init_device] no NODE_ID in config.json and no /mny/mtd/ipc/BurnUID
```

The BurnUID is the per-device serial number burned at the Augentix
factory (e.g. `ATPL-200007-TESTA`).  It can also be read via NetSDK:

```bash
curl -u admin: http://<ip>/netsdk/system/deviceinfo | python3 -m json.tool
# look for "serialNumber" field
```

Restore via telnet:

```bash
telnet <ip>
> root / adiance@999@arcisai
# echo 'ATPL-NNNNNN-XXXXX' > /mny/mtd/ipc/BurnUID
# sync
# reboot
```

Modern flash-kitty.py preserves+restores BurnUID automatically — this
recovery should only be needed on cameras flashed by older scripts or
recovered after manual `rm -rf /mny/mtd/ipc/*` mistakes.

### E. view-feed-fast.sh shows "Invalid data found when processing input"

This means the FLV pipeline got 0 bytes from SRT (or a corrupt stream).
Most likely cause: ap2p never reached `state_ready` (no retained config
on `/90`) or `ctrl_registered` (signaling REGISTER never completed).

Check fresh boot publish:

```bash
mosquitto_sub -h mqtt-staging.devices.arcisai.io -p 443 \
  -u Torque -P Raptor@0 \
  -t torque/tx/ATPL-NNNNNN-XXXXX/boot -W 60 -v
```

Expected milestones (in `since_ambicam_start_sec`):

- `ap2p_main` ≈ 0–13 s
- `config_loaded` ≈ ap2p_main + 0.1 s
- `mqtt_connected` ≈ 5–8 s after ap2p_main
- `state_ready` ≈ mqtt_connected (case 81 / 90 retained must arrive)
- `srt_started` ≈ 0–1 s after ap2p_main
- `ctrl_registered` ≈ 10–15 s after ap2p_main

If any milestone is missing, that subsystem failed.  See
`docs/MQTT_CASES.md` and `docs/ROADMAP.md` for diagnostics.

## How to recover from a fully bricked camera

If telnet, HTTP, RTSP are ALL dead, the camera is genuinely bricked.
This shouldn't happen with the hardened recipe, but if it does:

1. **Hard power-cycle** (unplug for 30 s, plug back).
2. **Factory reset** via HW button (10 s hold during boot).
3. **Wait 90 s** for vendor firmware to fully boot.
4. **Verify reachability** on the LAN (`ping`, then `nc -z` on 23, 80, 554).
5. **Re-flash from scratch**: `tools/flash-kitty.py --ip <ip> --tag <known-good>`.

If steps 1–4 don't restore reachability, the camera needs vendor RMA —
the bootloader or kernel partition was damaged, which the user-space
flash-kitty cannot do.  flash-kitty only writes to `/mny/mtd/ipc`
(mtdblock7), not boot/kernel/rootfs partitions.

## Pre-flash checklist (manual)

Run these before any production batch flash to avoid surprises:

```bash
# 1. tarball validation (no camera needed)
python3 -c "
import tarfile, sys
p = '/path/to/Augentix.tar.gz'
with tarfile.open(p) as tf:
    names = {n.removeprefix('AugenTix/') for n in tf.getnames() if n.startswith('AugenTix/')}
need = {'ap2p', 'ambicam.sh', 'config.json', 'VERSION', 'BUILT_AT', 'COMMIT', 'libpaho-mqtt3cs.so.1', 'libcurl.so.4'}
print('OK' if need <= names else f'MISSING: {need - names}')
"

# 2. mosquitto CLI installed
brew install mosquitto       # macOS
apt-get install mosquitto-clients   # Linux

# 3. gh CLI authenticated (for release download)
gh auth status

# 4. camera reachable
ping -c 2 -W 2 <ip>
nc -z -w 2 <ip> 80 && echo HTTP OK
nc -z -w 2 <ip> 23 && echo TELNET OK || echo "telnet down — will enable via NetAPI/R.SwitchTelnet"
```

## Where the snapshots live

Every flash-kitty run writes a JSON snapshot before touching the camera:

```
~/.cache/kitty-flash/snapshots/<SN>-<YYYYMMDD>-<HHMMSS>.json
```

Contents: pre-flash `VERSION`, `COMMIT`, `BurnUID`, `ls -la /mny/mtd/ipc/ambicam`,
`df` output.  This is your forensic trail if a flash goes sideways.

## Where the tarball cache lives

```
~/.cache/kitty-flash/<tag>/Augentix.tar.gz
~/.cache/kitty-flash/tar               # static busybox tar binary
```

To force a fresh download:

```bash
rm -rf ~/.cache/kitty-flash/v2.0.7-rc2
tools/flash-kitty.py --ip <ip> --tag v2.0.7-rc2
```
