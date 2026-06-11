# Firmware Update Tool — `Augentix.tar.gz` contract

The legacy firmware update tool that ships kitty onto an Augentix camera
follows this exact recipe (lifted from `kitty_process.txt`, the field
manual for first-flash deployments).  Our CI publishes
`Augentix.tar.gz` so the recipe works **byte-for-byte** for v2.x without
the tool needing any updates.

## The recipe the tool runs on a freshly-flashed camera

```sh
cd /tmp
wget http://prong.arcisai.io/protected/augentix/lib/tar
chmod +x tar
wget http://prong.arcisai.io/protected/Augentix/<series>/Augentix.tar.gz
./tar -xvf Augentix.tar.gz                      # → AugenTix/
cd AugenTix
chmod 777 *

mkdir -p /etc/jffs2/ambicam
mkdir -p /mny/mtd/ipc/ambicam

cp *.crt client.key   /etc/jffs2/ambicam        # mTLS material (Phase 2+)
cp *                  /mny/mtd/ipc/ambicam      # everything else

cd /mny/mtd/ipc/ambicam/
rm -rf *.crt client.key                         # certs only on /etc/jffs2
rm -rf ../ambicam.sh
mv ambicam.sh ../.                              # launcher → /mny/mtd/ipc/ambicam.sh
ls                                              # advisory: should show binary + ~9 files
cd ..

reboot
```

## What v2.0.3+ `Augentix.tar.gz` contains

Built by `.github/workflows/build-augentix.yml:stage firmware-tool bundle`.
**No symlinks** — see `docs/SPACE_BUDGET.md` for why.

```
AugenTix/
├── ambicam.sh                        # v2 watchdog + log rotator (~32 lines)
├── ap2p                              # single statically-merged binary (~663 KB)
├── config.json                       # MQTT bootstrap (broker URL + creds, 3 keys)
├── VERSION                           # "2.0.3" etc.
├── BUILT_AT                          # ISO-8601 build timestamp
├── COMMIT                            # git sha
├── libpaho-mqtt3cs.so.1              # real, 177 KB — MQTT client (DT_NEEDED name)
├── libcurl.so.4                      # real, 425 KB — HTTP client (DT_NEEDED name)
├── kitty.crt                         # ZERO-BYTE placeholder (KITTY_NO_TLS)
└── client.key                        # ZERO-BYTE placeholder (KITTY_NO_TLS)
```

### Why each piece is there

| Item | Purpose |
|---|---|
| `ap2p` | The whole P2P daemon — MQTT + signaling + SRT rendezvous in one process. |
| `ambicam.sh` | Process supervisor + log rotator. Restarts `ap2p` if it crashes; copy-truncates `ap2p.log` at 256 KB. |
| `config.json` | MQTT broker URL + creds. Read once at startup. |
| `VERSION` / `BUILT_AT` / `COMMIT` | Audit trail. Operators read these via telnet to confirm what's installed. |
| `libpaho-mqtt3cs.so.1` + `libcurl.so.4` | Real upstream .so files that `ap2p`'s ELF needs at runtime. Named exactly as `DT_NEEDED` requests so the loader finds them directly. Shipped so a factory-reset camera always has them. |
| `kitty.crt` / `client.key` | **Empty placeholders.** Exist purely so the firmware tool's `cp *.crt client.key /etc/jffs2/ambicam` step doesn't fail. v2.0 is KITTY_NO_TLS — neither file is read. v2.1 (mTLS rollout) will replace these with real per-device cert + key. |

### What was removed in v2.0.3

The v2.0.0/v2.0.1 bundles shipped nine entries that broke the firmware tool on real cameras:

| Removed | Why |
|---|---|
| `libpaho-mqtt3cs.so.1.3.14` (the real file) + the `libpaho-mqtt3cs.so.1` symlink to it | Merged: the real .so is now named `libpaho-mqtt3cs.so.1` directly. Busybox `cp *` follows symlinks → was double-copying the 177 KB payload. |
| `libap2p_msg.so.1` / `libap2p_net.so.1` / `libap2p_tls.so.1` / `libap2p_crypto.so.1` symlinks | Reserved for v2.1 Phase F (when `patchelf` rewrites `DT_NEEDED` to the branded names). Until then, the dynamic loader never consults them — and busybox `cp *` was copying the underlying lib (or worse, `/usr/lib/libssl.so.3` which is 600+ KB) once per symlink, exhausting `/mny/mtd/ipc`. |
| `libssl.so.1.1` / `libcrypto.so.1.1` symlinks | OpenSSL 1.1 and 3 have incompatible ABIs — these symlinks were both unnecessary (nothing on v2 cameras looks for `.so.1.1`) and dangerous (anything that did would crash on symbol resolution). |
| `kitty.logrotate` | Camera busybox has no `logrotate` binary; the file listed v1 log filenames that don't exist in v2; v2's `ambicam.sh` does in-script rotation natively. Triple-dead. |

Net effect on `/mny/mtd/ipc/ambicam` after `cp *`: ~1.27 MB instead of ~4.5 MB. Comfortably fits the 1.5 MB partition.

## Things the tool author should know

- **The `ls` count check is advisory only.** v2.0.3 ships 10 entries (vs v1's "9 files and binary"). Don't fail the deploy on count mismatch.
- **`cp *.crt client.key` always succeeds in v2.x** because we ship empty placeholders. No behavior change needed in the tool.
- **No reboot script change needed.** After the standard `reboot` step, `/etc/init.d/S00mount.sh` invokes `/mny/mtd/ipc/ambicam.sh` exactly as before; the new launcher in turn spawns `ap2p`.
- **MQTT retained config** (the case-81 payload) is operator-controlled on the broker side — it is NOT part of this tarball. Operators publish it via `ops/fleet-provision.sh` (batch) or `ops/cutover-v2.0.sh` (single unit). The on-camera filesystem stays the same regardless.

## Recovery on a camera that hit "No space left" during v2.0.1 install

If a v2.0.1 install partially failed with `cp: No space left on device`, the camera is in a half-installed state with garbage in `/mny/mtd/ipc/ambicam`. To recover:

```sh
# On the camera (via telnet)
killall -9 ap2p 2>/dev/null
rm -rf /mny/mtd/ipc/ambicam /mny/mtd/ipc/ambicam.sh
sync
df -h /mny/mtd/ipc                              # confirm partition reclaimed
# Re-run the standard firmware-tool recipe with v2.0.3+ tarball
```

v2.0.3+ never hits the failure mode because `cp *` only walks real files.

## Versioning convention

| Series tag | What's inside |
|---|---|
| `A_Series_0.12.3` | v1.13.2 (legacy two-binary layout — provider_srt + MQTT_vcamclient_Augentix) |
| `A_Series_2.0.x` | v2.x single-binary `ap2p` |

The firmware tool URL pattern stays:
```
http://prong.arcisai.io/protected/Augentix/<A_Series_X.Y.Z>/Augentix.tar.gz
```

Upload `Augentix.tar.gz` from the GitHub Release to the corresponding
path on `prong.arcisai.io` when cutting a new firmware revision.
