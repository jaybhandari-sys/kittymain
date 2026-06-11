#!/usr/bin/env python3
"""End-to-end SRT pipeline test, with the consumer running on the CLOUD
server so SRT rendezvous succeeds (cloud has a public IP, no NAT path on
the consumer side).  This isolates the SRT pipe + consumer FLV-serving
logic from any phone/MediaCodec side-effects.

Steps:
  1. (re)deploy + start provider_srt on the camera via telnet.
  2. SSH into cloud, push consumer_srt_p2p binary, launch it.
  3. SSH into cloud, curl 60s of HTTP-FLV from localhost into /tmp/cap.flv.
  4. SCP the capture back to /tmp on this WSL host.
  5. Kill cloud-side consumer.
  6. Walk the captured FLV byte-by-byte locally and report:
        - tag count, total bytes
        - byte where a desync first appears (if any)
        - AVC/AAC sequence-header re-emissions (count > 1 = bug)
        - PTS regressions / large gaps
        - NAL unit type distribution
"""
import os, subprocess, sys, time, signal
from pathlib import Path

THIS = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS))
from deploy_to_camera_telnet import Telnet, login, CAM  # type: ignore

CLOUD_HOST   = "142.93.223.221"
CLOUD_USER   = "root"
CLOUD_PASS   = "toR@8155que"
SERVICE_ID   = "cam001"
SIGNALING    = f"{CLOUD_HOST}:8888"
STUN         = f"{CLOUD_HOST}:3478"
LOCAL_LISTEN = "127.0.0.1:9099"
HTTP_PATH    = "/flv/live.flv"
CAPTURE_SEC  = 60

LOCAL_BIN    = THIS / "binaries" / "consumer_srt_p2p.static"
LOCAL_CAP    = Path("/tmp/cloud_e2e_cap.flv")


def ssh(cmd: str, timeout=30, get_output=True):
    full = ["sshpass", "-p", CLOUD_PASS, "ssh", "-o", "StrictHostKeyChecking=no",
            f"{CLOUD_USER}@{CLOUD_HOST}", cmd]
    if get_output:
        return subprocess.run(full, capture_output=True, text=True, timeout=timeout)
    return subprocess.run(full, timeout=timeout)


def scp(src_local, dst_remote):
    return subprocess.run(
        ["sshpass", "-p", CLOUD_PASS, "scp", "-o", "StrictHostKeyChecking=no",
         src_local, f"{CLOUD_USER}@{CLOUD_HOST}:{dst_remote}"],
        capture_output=True, text=True, timeout=60)


def scp_back(src_remote, dst_local):
    return subprocess.run(
        ["sshpass", "-p", CLOUD_PASS, "scp", "-o", "StrictHostKeyChecking=no",
         f"{CLOUD_USER}@{CLOUD_HOST}:{src_remote}", dst_local],
        capture_output=True, text=True, timeout=60)


def banner(s):
    print("\n" + "=" * 70 + f"\n  {s}\n" + "=" * 70, flush=True)


def step1_start_provider():
    banner("STEP 1 / Telnet camera + start provider_srt")
    t = Telnet(CAM, timeout=8)
    try:
        login(t)
        t.run("killall provider_srt 2>/dev/null; rm -f /tmp/provider_srt.log; sync")
        time.sleep(0.3)
        t.send("cd /tmp && ./provider_srt provider_srt.conf > /tmp/provider_srt.log 2>&1 &")
        time.sleep(0.5)
        out = t.run("sleep 3; ps w | grep provider_srt | grep -v grep; echo --LOG--; cat /tmp/provider_srt.log 2>/dev/null | tail -25", timeout=12)
        print(out)
    finally:
        t.close()


def step2_launch_cloud_consumer():
    banner("STEP 2 / Push + launch consumer on cloud")
    print("scp consumer binary…")
    r = scp(str(LOCAL_BIN), "/tmp/consumer_srt_p2p")
    if r.returncode != 0:
        print("scp failed:", r.stderr); sys.exit(2)
    ssh("chmod +x /tmp/consumer_srt_p2p && killall consumer_srt_p2p 2>/dev/null; rm -f /tmp/consumer.log /tmp/cap.flv; true")
    print("launching consumer in background…")
    cmd = (
        f"nohup /tmp/consumer_srt_p2p "
        f"--signaling={SIGNALING} --stun={STUN} "
        f"--service-id={SERVICE_ID} --listen={LOCAL_LISTEN} "
        f"--http-path={HTTP_PATH} --latency-ms=300 "
        f"> /tmp/consumer.log 2>&1 & echo PID=$!"
    )
    r = ssh(cmd, timeout=10)
    print(r.stdout.strip())
    # Wait for the consumer to log "http: listening" — this is the only
    # reliable signal that rendezvous + HTTP listener both came up.  A bare
    # nc -z 9099 probe is unreliable: a stale subprocess from a previous
    # test may still be holding 9099 even though it's not the one we just
    # spawned.
    print("waiting for cloud consumer's 'http: listening' log line…")
    for i in range(40):  # 40 * 0.5s = 20s
        r = ssh("grep -c 'http: listening' /tmp/consumer.log 2>/dev/null", timeout=5)
        if r.stdout.strip().isdigit() and int(r.stdout.strip()) > 0:
            print(f"  rendezvous + listener up after {(i+1)*0.5:.1f}s")
            break
        time.sleep(0.5)
    else:
        log = ssh("tail -40 /tmp/consumer.log", timeout=5).stdout
        print("FAIL: consumer never reached 'http: listening' state.  Last 40 log lines:")
        print(log)
        # Also dump camera-side log
        sys.exit(2)
    log = ssh("head -20 /tmp/consumer.log", timeout=5).stdout
    print("---- consumer.log (head) ----")
    print(log.strip())


def step3_capture():
    banner(f"STEP 3 / Capture {CAPTURE_SEC}s of HTTP-FLV on cloud")
    cmd = f"timeout {CAPTURE_SEC + 5} curl -sS -N --max-time {CAPTURE_SEC} 'http://{LOCAL_LISTEN}{HTTP_PATH}' -o /tmp/cap.flv; ls -la /tmp/cap.flv"
    r = ssh(cmd, timeout=CAPTURE_SEC + 30)
    print(r.stdout.strip())
    if r.stderr.strip(): print("stderr:", r.stderr.strip())
    # also tail the consumer log
    log = ssh("tail -20 /tmp/consumer.log", timeout=5).stdout
    print("---- consumer.log (tail) ----")
    print(log.strip())


def step4_pull_back():
    banner("STEP 4 / Pull capture back to WSL")
    if LOCAL_CAP.exists(): LOCAL_CAP.unlink()
    r = scp_back("/tmp/cap.flv", str(LOCAL_CAP))
    if r.returncode != 0:
        print("scp back failed:", r.stderr); sys.exit(2)
    sz = LOCAL_CAP.stat().st_size
    print(f"local capture: {LOCAL_CAP} ({sz} bytes)")
    return sz


def step5_cleanup():
    banner("STEP 5 / Stop cloud-side consumer")
    ssh("killall consumer_srt_p2p 2>/dev/null; true", timeout=5)


# ── Step 6: byte-level FLV walker (same logic as local_e2e_test.py) ──────────
def step6_analyse():
    banner("STEP 6 / FLV byte-level analysis")
    data = LOCAL_CAP.read_bytes()
    n = len(data)
    if n < 13:
        print("FAIL: capture too short"); return
    if data[:3] != b"FLV":
        print(f"FAIL: not FLV (first 8 bytes: {data[:8].hex()})"); return
    print(f"FLV header: ver={data[3]} flags=0x{data[4]:02x} headerSize={int.from_bytes(data[5:9],'big')}")
    pts0 = int.from_bytes(data[9:13], "big")
    print(f"PreviousTagSize0 = {pts0} (must be 0)")
    pos = 13
    tag_idx = 0
    last_pts_video = -1; last_pts_audio = -1
    avc_seq_count = 0; aac_seq_count = 0; keyframe_count = 0
    pts_jumps = []
    desync_at = None
    nal_types = {}
    audio_tags = video_tags = script_tags = 0
    first_keyframe_at = None
    bytes_seen_video = 0; bytes_seen_audio = 0
    while pos < n:
        if pos + 11 > n:
            print(f"!! tag #{tag_idx} truncated header @byte {pos}/{n}")
            desync_at = pos; break
        tag_type  = data[pos]
        data_size = int.from_bytes(data[pos+1:pos+4], "big")
        timestamp = int.from_bytes(data[pos+4:pos+7], "big") | (data[pos+7] << 24)
        if tag_type not in (8, 9, 18):
            print(f"!! tag #{tag_idx} @byte {pos}: bad tag_type={tag_type} size={data_size} ts={timestamp}  (PRIOR TAG END = byte {pos})")
            print("    Hex dump of 16 bytes around this position:")
            lo = max(0, pos - 4); hi = min(n, pos + 12)
            print(f"    [{lo}-{hi}]: {data[lo:hi].hex()}")
            desync_at = pos; break
        body_start = pos + 11
        body_end   = body_start + data_size
        if body_end + 4 > n:
            print(f"!! tag #{tag_idx} body truncated @byte {body_start} (need {data_size}, have {n-body_start})")
            desync_at = pos; break
        body = data[body_start:body_end]
        prev_tag_size = int.from_bytes(data[body_end:body_end+4], "big")
        expected_prev = data_size + 11
        if prev_tag_size != expected_prev:
            print(f"!! tag #{tag_idx} @byte {pos}: PreviousTagSize mismatch got={prev_tag_size} expected={expected_prev}  ts={timestamp} type={tag_type}")
            desync_at = pos; break

        if tag_type == 9:
            video_tags += 1; bytes_seen_video += data_size
            if data_size >= 1:
                frame_type = (body[0] & 0xF0) >> 4
                codec_id   = body[0] & 0x0F
                if codec_id == 7 and data_size >= 2:
                    avc_pkt_type = body[1]
                    if avc_pkt_type == 0:
                        avc_seq_count += 1
                        if tag_idx > 5:
                            print(f"!! tag #{tag_idx} @byte {pos}: AVC SEQ HEADER re-emission at PTS {timestamp} (count={avc_seq_count})")
                    elif avc_pkt_type == 1:
                        p = 5
                        while p + 4 <= data_size:
                            nl = int.from_bytes(body[p:p+4], "big")
                            if p + 4 + nl > data_size:
                                print(f"!! tag #{tag_idx} @byte {pos}: NAL length {nl} overflows tag (remaining={data_size-p-4})")
                                desync_at = pos; break
                            if nl > 0:
                                nt = body[p+4] & 0x1F
                                nal_types[nt] = nal_types.get(nt, 0) + 1
                            p += 4 + nl
                if frame_type == 1:
                    keyframe_count += 1
                    if first_keyframe_at is None: first_keyframe_at = (tag_idx, timestamp, pos)
            if last_pts_video >= 0 and timestamp < last_pts_video:
                pts_jumps.append((tag_idx, "video-back", last_pts_video, timestamp))
            if last_pts_video >= 0 and timestamp > last_pts_video + 5000:
                pts_jumps.append((tag_idx, "video-fwd", last_pts_video, timestamp))
            last_pts_video = timestamp
        elif tag_type == 8:
            audio_tags += 1; bytes_seen_audio += data_size
            if data_size >= 2 and (body[0] >> 4) == 10 and body[1] == 0:
                aac_seq_count += 1
            if last_pts_audio >= 0 and timestamp < last_pts_audio:
                pts_jumps.append((tag_idx, "audio-back", last_pts_audio, timestamp))
            last_pts_audio = timestamp
        else:
            script_tags += 1

        pos = body_end + 4
        tag_idx += 1

    print()
    print(f"  bytes total              : {n}")
    print(f"  tags parsed              : {tag_idx} (video={video_tags} audio={audio_tags} script={script_tags})")
    print(f"  desync first appears at  : {'byte ' + str(desync_at) if desync_at is not None else '(NONE — clean to EOF)'}")
    print(f"  AVC sequence-headers     : {avc_seq_count} (expected: 1)")
    print(f"  AAC sequence-headers     : {aac_seq_count} (expected: 0 or 1)")
    print(f"  keyframes                : {keyframe_count}")
    if first_keyframe_at:
        print(f"  first keyframe           : tag #{first_keyframe_at[0]} pts={first_keyframe_at[1]}ms byte={first_keyframe_at[2]}")
    print(f"  last video PTS (ms)      : {last_pts_video}")
    print(f"  last audio PTS (ms)      : {last_pts_audio}")
    print(f"  PTS irregularities       : {len(pts_jumps)}")
    for j in pts_jumps[:15]:
        print(f"      tag={j[0]:>5}  kind={j[1]:<11}  prev={j[2]}  cur={j[3]}  delta={j[3]-j[2]}")
    if pts_jumps[15:]:
        print(f"      ... and {len(pts_jumps)-15} more")
    print(f"  NAL types histogram      : {dict(sorted(nal_types.items()))}")
    print("    (1=non-IDR, 5=IDR/keyframe, 6=SEI, 7=SPS, 8=PPS, 9=AUD)")

    print()
    print("Verdict:")
    if desync_at is not None:
        print(f"  ✗ Stream has a desync at byte {desync_at}.  The SRT pipe or consumer FLV-serving logic is corrupting the byte stream.  Bug is server-side.")
    elif avc_seq_count > 1:
        print(f"  ⚠ AVC sequence header re-emitted {avc_seq_count} times — this can confuse some decoders.")
    elif len(pts_jumps) > 0:
        print(f"  ⚠ {len(pts_jumps)} PTS irregularities — some decoders will stall on these.")
    else:
        print(f"  ✓ Stream is byte-clean over {CAPTURE_SEC}s.  No desync, no header re-emission, no PTS jumps.  If the phone still stalls, bug is in Jessibuca/MediaCodec, NOT in the SRT pipe.")


def main():
    try:
        step1_start_provider()
        time.sleep(2)
        step2_launch_cloud_consumer()
        step3_capture()
        sz = step4_pull_back()
        if sz < 1024:
            print("FAIL: capture suspiciously small")
            return 1
        step6_analyse()
        return 0
    finally:
        step5_cleanup()


if __name__ == "__main__":
    sys.exit(main() or 0)
