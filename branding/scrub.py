#!/usr/bin/env python3
"""
branding/scrub.py — Phase D string-literal scrubber for the ap2p binary.

Reads the (already stripped) ELF and zero-fills every occurrence of a
forbidden token in `.rodata` / `.data` so that `strings <binary> | grep`
no longer surfaces it.  Run AFTER `strip --strip-all` and the .comment /
.note section removal so we only operate on data sections, not metadata.

Why zero-fill, not byte-replace?

  C string literals in .rodata are NUL-terminated runs.  Replacing
  "OpenSSL 3.0.16 ..." with NUL bytes turns the literal into an empty
  string ("").  Any printf("%s", SSL_get_version()) now emits the empty
  string.  Code keeps working; only the visible byte pattern goes away.
  No length change → ELF offsets stay intact, no need to relink.

  We *could* substitute branded names ("ap2p-tls/1.0\0...") of equal
  length, but the simpler choice is safer and the post-strip image is
  small enough that we don't lose much info value.

Forbidden patterns:

  * Upstream library banners — OpenSSL, libcurl, paho-mqtt, libsrt,
    libjuice, libevent, Mosquitto
  * Augentix (vendor-revealing strings in paths/build info)
  * GCC: (compiler banner; .comment is already gone, but DWARF or
    embedded help strings may still hold one)
  * Old config-key names — SIGNALING_HOST, STUN_, TURN_, SERVICE_ID,
    provider_srt.  v2.0 doesn't use these as keys (we use CTRL_*,
    EDGE_*, RELAY_*, NODE_ID, ap2p.conf is in-memory anyway).  Any
    occurrence in the binary is a vestige from the verbatim HTTP.c
    copy and will leak protocol identity to a casual `strings`.

Usage:

    python3 branding/scrub.py <path/to/ap2p>

Exit code 0 on success, non-zero on read/write error.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


# Length-preserving zero-fills (case-INsensitive).  We scrub the BYTES
# matching these patterns and any NUL-terminated suffix that hangs off
# them up to the next non-printable / NUL — that catches the "OpenSSL
# 3.0.16 22 Sep 2024" trailing version+date that follows the bare
# "OpenSSL" token in OpenSSL's banner.

# Each tuple: (regex_bytes, label_for_log).  Patterns are bytes-objects
# because we operate on the raw ELF file.

# Banner patterns — designed to NOT match DT_NEEDED soname strings.  The
# guard in scrub() also rejects anything matching ^lib...\.so(\.N)*$ as a
# final safety net, but anchoring the patterns to banner-format prefixes
# keeps the scrub footprint tight.
FORBIDDEN: list[tuple[bytes, str]] = [
    (rb"(?i)OpenSSL [0-9][^\x00]*",       "OpenSSL banner"),
    (rb"(?i)libcurl[/\- ][^\x00]*",       "libcurl banner"),
    (rb"(?i)Eclipse Paho[^\x00]*",        "Eclipse Paho banner"),
    (rb"(?i)libpaho-mqtt-c[/\- ][^\x00]*","libpaho banner"),
    (rb"(?i)paho-mqtt-c[/\- ][^\x00]*",   "paho-mqtt banner"),
    (rb"(?i)Mosquitto[^\x00]*",           "Mosquitto banner"),
    (rb"(?i)libsrt[/\- ][^\x00]*",        "libsrt banner"),
    (rb"(?i)libjuice[/\- ][^\x00]*",      "libjuice banner"),
    (rb"(?i)libevent[/\- ][^\x00]*",      "libevent banner"),
    (rb"(?i)Augentix[^\x00]*",            "Augentix vendor string"),
    (rb"GCC:[^\x00]*",                    "GCC build banner"),
    # Legacy v1 config-key vocabulary — UPPER-case only.  The lower-case form
    # (e.g. `service_id=%s`) is the wire-protocol field name that the
    # signaling server expects and CANNOT be renamed without server-side work.
    (rb"SIGNALING_HOST[^\x00]*",         "legacy SIGNALING_HOST key"),
    (rb"STUN_(HOST|PORT|USERNAME|PASSWORD)[^\x00]*",  "legacy STUN_* key"),
    (rb"TURN_(HOST|PORT|USERNAME|PASSWORD)[^\x00]*",  "legacy TURN_* key"),
    (rb"SERVICE_ID[^\x00]*",             "legacy SERVICE_ID key (upper-case only)"),
    (rb"provider_srt[^\x00]*",           "legacy provider_srt path"),
]


# A shared-object filename has the form `lib<NAME>.so` or `lib<NAME>.so.<N>`
# or `lib<NAME>.so.<N>.<M>(.<...>)`.  Skip these — they are .dynstr
# DT_NEEDED entries that the dynamic loader resolves by exact byte
# comparison.  Zero-filling them produces empty NEEDED entries and the
# loader segfaults before main() runs.
_SO_FILENAME = re.compile(rb"^lib[A-Za-z0-9_+\.\-]+\.so(\.[0-9]+)*$")


def scrub(path: Path) -> int:
    data = bytearray(path.read_bytes())
    total_scrubbed = 0
    total_hits = 0
    total_skipped = 0

    for pat, label in FORBIDDEN:
        compiled = re.compile(pat)
        hits_this_pat = 0
        bytes_this_pat = 0
        skipped_this_pat = 0
        for m in compiled.finditer(bytes(data)):  # iterate over an immutable snapshot
            start, end = m.start(), m.end()
            matched = bytes(data[start:end])
            # CRITICAL guard: never zero-fill a string that looks like an
            # ELF dependency soname (libXXX.so.N).  uClibc's loader matches
            # NEEDED entries by exact bytes; empty NEEDED → segfault.
            if _SO_FILENAME.match(matched):
                skipped_this_pat += 1
                continue
            # Zero-fill the matched run.  Preserves the trailing NUL byte that
            # closes the C string (we matched [^\x00]*, so end points at the NUL).
            for i in range(start, end):
                data[i] = 0
            hits_this_pat += 1
            bytes_this_pat += (end - start)
        if hits_this_pat or skipped_this_pat:
            note = f"  scrubbed {hits_this_pat} × {label} ({bytes_this_pat} bytes)"
            if skipped_this_pat:
                note += f" — kept {skipped_this_pat} soname-like match(es) intact"
            print(note)
            total_hits += hits_this_pat
            total_scrubbed += bytes_this_pat
            total_skipped += skipped_this_pat

    if total_hits == 0 and total_skipped == 0:
        print("  (nothing to scrub — binary is already brand-clean)")
    else:
        print(f"  total: {total_hits} scrubbed, {total_scrubbed} bytes zeroed, "
              f"{total_skipped} soname-like matches preserved")

    path.write_bytes(bytes(data))
    return 0


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <path-to-binary>", file=sys.stderr)
        return 1
    p = Path(argv[1])
    if not p.is_file():
        print(f"error: {p} is not a file", file=sys.stderr)
        return 1
    print(f"branding/scrub: processing {p} ({p.stat().st_size} bytes)")
    return scrub(p)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
