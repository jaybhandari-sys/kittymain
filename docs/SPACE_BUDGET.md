# On-camera flash budget — `/mny/mtd/ipc/ambicam` and friends

The Augentix HC1703 (Cortex-A7, uClibc) has a tight flash layout.  This doc
captures what fits where, what blew up in v2.0.1, and what we changed in
v2.0.3.

## Partition map (typical HC1703 / similar boards)

Run `df -h` on a deployed camera to get the actual numbers for that SKU.
Order-of-magnitude figures across the boards we've handled:

| Mount | FS | Size | Usage |
|---|---|---|---|
| `/etc/jffs2` | jffs2 | **8–16 MB** | System config, mTLS certs/keys, persistent device identity.  Larger because it holds the rootfs settings. |
| `/mny/mtd/ipc` | jffs2 | **1.5–4 MB** | Application binaries + runtime state.  Tight.  This is where `ap2p` lives. |
| `/mnt/tf` | vfat (SD card) | 0–N GB | OPTIONAL — may not be mounted at all.  Cannot be depended on. |

The legacy firmware-update-tool recipe (`kitty_process.txt`, archived in
`ops/firmware-update-tool.md`) puts:
- certs/keys → `/etc/jffs2/ambicam/`  (small, durable)
- binaries  → `/mny/mtd/ipc/ambicam/` (large, replaceable)
- launcher  → `/mny/mtd/ipc/ambicam.sh` (one level up from the binaries)

That split is reasonable.  The problem is just that v2's binaries are
bigger than v1's were.

## v2.0.3 on-flash budget (`/mny/mtd/ipc/ambicam/`)

After the firmware-tool recipe runs on a v2.0.3 bundle:

| File | Size | Notes |
|---|---|---|
| `ap2p` | 663 KB | Single binary — MQTT + SRT + signaling fused. |
| `libcurl.so.4` | 425 KB | Real upstream .so.  DT_NEEDED of ap2p. |
| `libpaho-mqtt3cs.so.1` | 177 KB | Real upstream .so.  DT_NEEDED of ap2p. |
| `config.json` | < 1 KB | MQTT bootstrap only.  3 keys read; others ignored. |
| `VERSION` / `BUILT_AT` / `COMMIT` | < 1 KB | Provenance. |
| **Total binaries + config** | **~1.27 MB** | |
| `ap2p.log` (live) | ≤ 256 KB | Watchdog truncates at this cap. |
| `ap2p.log.old` | ≤ 256 KB | One prior rotation. |
| **Total worst-case steady-state** | **~1.78 MB** | |

On a 1.5 MB partition this is over budget.  Mitigations:

1. Default `VERBOSE=0` in the retained MQTT payload → keeps log churn low,
   so `ap2p.log` typically sits at < 50 KB and the .old rotation rarely
   exists.  Realistic footprint ~1.32 MB on a 1.5 MB partition.
2. If a specific camera enables `VERBOSE=1` for debugging and approaches
   the cap, the watchdog still rotates correctly; the camera doesn't
   crash, but flash headroom is tight.  Re-disable verbose ASAP.
3. **Future option (v2.1):** move `libcurl.so.4` + `libpaho-mqtt3cs.so.1`
   to `/etc/jffs2/ambicam/` and set `RPATH=/etc/jffs2/ambicam` on `ap2p`.
   That frees ~600 KB on `/mny/mtd/ipc/ambicam/` and gives `ap2p.log` all
   the headroom it could want.  Requires a firmware-tool recipe change.

## v2.0.1 failure forensics

A real install on the field camera failed with:

```
[root@Root:/tmp/AugenTix]# cp *                /mny/mtd/ipc/ambicam
cp: write error: No space left on device
cp: can't create '/mny/mtd/ipc/ambicam/libap2p_msg.so.1'   : No space left on device
cp: can't create '/mny/mtd/ipc/ambicam/libap2p_net.so.1'   : No space left on device
cp: can't create '/mny/mtd/ipc/ambicam/libap2p_tls.so.1'   : No space left on device
cp: can't create '/mny/mtd/ipc/ambicam/libcrypto.so.1.1'   : No space left on device
cp: can't create '/mny/mtd/ipc/ambicam/libcurl.so.4'       : No space left on device
cp: can't create '/mny/mtd/ipc/ambicam/libpaho-mqtt3cs.so.1': No space left on device
cp: can't create '/mny/mtd/ipc/ambicam/libpaho-mqtt3cs.so.1.3.14': No space left on device
cp: can't create '/mny/mtd/ipc/ambicam/libssl.so.1.1'      : No space left on device
```

### Root cause

Busybox `cp` (no `-d` / `-P` / `-a` flags) **follows symlinks by default**.
The v2.0.1 bundle shipped seven symlinks, each pointing at a real file the
camera already had loaded or that ap2p didn't actually need.  `cp *` walked
them in alphabetical order and tried to copy the dereferenced contents
into `/mny/mtd/ipc/ambicam/` *under the symlink's name*:

| Bundle entry | Was | Cp tried to write |
|---|---|---|
| `libap2p_crypto.so.1` | symlink → `/usr/lib/libcrypto.so.3` | ≈ 1.6 MB copy of system libcrypto |
| `libap2p_msg.so.1`    | symlink → `libpaho-mqtt3cs.so.1.3.14` | ≈ 177 KB duplicate of paho |
| `libap2p_net.so.1`    | symlink → `libcurl.so.4` | ≈ 425 KB duplicate of curl |
| `libap2p_tls.so.1`    | symlink → `/usr/lib/libssl.so.3` | ≈ 600 KB copy of system libssl |
| `libcrypto.so.1.1`    | symlink → `/usr/lib/libcrypto.so.3` | ANOTHER ≈ 1.6 MB libcrypto copy |
| `libpaho-mqtt3cs.so.1` | symlink → `libpaho-mqtt3cs.so.1.3.14` | ANOTHER paho duplicate |
| `libssl.so.1.1`       | symlink → `/usr/lib/libssl.so.3` | ANOTHER libssl copy |
| `libpaho-mqtt3cs.so.1.3.14` | real file 177 KB | one real copy |
| `libcurl.so.4`        | real file 425 KB | one real copy |
| `ap2p`                | real file 663 KB | one real copy |

Total payload `cp *` was trying to write: ≈ 4.5 MB.  Partition: 1.5 MB.
ENOSPC at the first symlink dereference past the early small files, exact
file dependent on alphabetical scheduling.

### Fix (v2.0.3)

Stop shipping symlinks in the bundle entirely.  Name the real .so files
exactly as `ap2p`'s `DT_NEEDED` requests them (`libpaho-mqtt3cs.so.1` and
`libcurl.so.4`).  Drop the branded `libap2p_*.so.1` placeholders until v2.1
Phase F actually rewrites `DT_NEEDED`.  Drop the `libssl.so.1.1` / `libcrypto.so.1.1` ABI-mismatched legacy symlinks (nothing on v2 cameras looks for them and crashing v1.13.x leftovers is not a goal).  Drop `kitty.logrotate` (camera has no `logrotate`).

### Recovery on a half-installed camera

If a v2.0.1 install ran out of space partway through `cp *`, the camera
has garbage in `/mny/mtd/ipc/ambicam/`.  To recover:

```sh
killall -9 ap2p 2>/dev/null
rm -rf /mny/mtd/ipc/ambicam /mny/mtd/ipc/ambicam.sh
sync
df -h /mny/mtd/ipc                                 # confirm reclaim
# Then re-run the standard firmware-tool recipe with a v2.0.3+ tarball.
```

v2.0.3+ doesn't hit this failure mode.

## Why we don't statically link everything

The compressed binary `ap2p` is already 663 KB.  Linking libcurl + paho-mqtt
+ libsrt + libjuice statically would push it past 2 MB, which exceeds
`/mny/mtd/ipc` even with the rest of the bundle removed.  Dynamic linking
against the two upstream .so files (177 KB + 425 KB) is the only path
that fits the partition at all.  libssl/libcrypto come from `/usr/lib`
(supplied by Augentix's rootfs) — we don't ship those.
