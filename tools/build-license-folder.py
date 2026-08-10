#!/usr/bin/env python3
"""
Build a ready-to-drop ux0/license folder from NoPayStation-style .tsv databases.

NoPayStation (and similar) publish tab-separated files (PSV_GAMES.tsv,
PSV_DLCS.tsv, ...) whose rows include a "Content ID" and a "zRIF" string. The
zRIF is the compressed form of a NoNpDrm .rif license. This script decodes each
zRIF into a .rif and files it exactly where the emulator looks for it:

    <out>/<TITLE_ID>/<CONTENT_ID>.rif        (TITLE_ID = content_id[7:16])

so you can copy the resulting folder straight to  ux0/license  and the emulator's
get_license() finds every game's and DLC's key automatically -- no per-title
hunting. The zRIF->rif math here is byte-identical to the emulator's own
zrif2rif (same 1024-byte dictionary, zlib wbits=10); validated by round-trip.

Usage:
    python3 build-license-folder.py PSV_GAMES.tsv PSV_DLCS.tsv --out ./license
    # then copy ./license into your ux0/  (merges with any existing licenses)

Only games you legally own should be used; this tool just converts key formats.
"""

import argparse
import base64
import csv
import os
import sys
import zlib

# The 1024-byte zRIF dictionary, lifted verbatim from psvpfstools libzrif
# (keyflate.c g_dict). Embedded so this script has no external dependencies.
_DICT_B64 = (
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAwMDA5AAAAAAAAAAAAAAAwMDAwNjAwMDA3MDAwMDgAMDAwMDMwMDAwNDAwMDA1MF8wMC1BRERDT05UMDAwMDItUENTRzAwMDAwMDAwMDAxLVBDU0UwMDAtUENTRjAwMC1QQ1NDMDAwLVBDU0QwMDAtUENTQTAwMC1QQ1NCMDAwAAEAAQABAALvzauJZ0UjAQ=="
)
_DICT = base64.b64decode(_DICT_B64)
assert len(_DICT) == 1024, len(_DICT)

_RIF_SIZE = 512
_SKIP_VALUES = {"", "missing", "not required", "n/a", "-"}


def zrif_to_rif(zrif: str) -> bytes:
    """Decode a zRIF string to raw .rif bytes (mirror of psvpfstools zrif2rif)."""
    raw = base64.b64decode(zrif + "=" * (-len(zrif) % 4))
    d = zlib.decompressobj(wbits=10, zdict=_DICT)
    out = d.decompress(raw, _RIF_SIZE) + d.flush()
    return out


def find_cols(header):
    """Return (content_id_idx, zrif_idx) from a header row, case-insensitively."""
    lower = [h.strip().lower() for h in header]
    cid = zr = None
    for i, h in enumerate(lower):
        if h in ("content id", "contentid", "content_id"):
            cid = i
        elif h == "zrif" or h.endswith(" zrif"):
            zr = i
    return cid, zr


def process(tsv_path, out_dir, stats):
    with open(tsv_path, encoding="utf-8", errors="replace", newline="") as fh:
        reader = csv.reader(fh, delimiter="\t")
        try:
            header = next(reader)
        except StopIteration:
            print(f"  (empty file: {tsv_path})")
            return
        cid_i, zr_i = find_cols(header)
        if cid_i is None or zr_i is None:
            print(f"  !! {os.path.basename(tsv_path)}: could not find 'Content ID' and 'zRIF' columns "
                  f"(found: {', '.join(header)})")
            stats["bad_files"] += 1
            return
        for row in reader:
            if len(row) <= max(cid_i, zr_i):
                continue
            content_id = row[cid_i].strip()
            zrif = row[zr_i].strip()
            if len(content_id) < 16:
                continue
            if zrif.lower() in _SKIP_VALUES:
                stats["no_zrif"] += 1
                continue
            title_id = content_id[7:16]
            try:
                rif = zrif_to_rif(zrif)
            except Exception as e:  # noqa: BLE001
                stats["errors"] += 1
                if stats["errors"] <= 10:
                    print(f"  !! {content_id}: {e}")
                continue
            if len(rif) != _RIF_SIZE:
                stats["errors"] += 1
                if stats["errors"] <= 10:
                    print(f"  !! {content_id}: unexpected rif size {len(rif)} (expected {_RIF_SIZE})")
                continue
            dest_dir = os.path.join(out_dir, title_id)
            os.makedirs(dest_dir, exist_ok=True)
            with open(os.path.join(dest_dir, content_id + ".rif"), "wb") as out:
                out.write(rif)
            stats["written"] += 1


def _looks_like_tsv(name):
    low = name.lower()
    return low.endswith(".tsv") or low.endswith(".tsv.txt")


def resolve_inputs(items, stats):
    """Expand folders, tolerate a browser-added .txt, and hint the real names on a miss."""
    resolved = []
    for it in items:
        if os.path.isdir(it):
            found = [os.path.join(it, n) for n in sorted(os.listdir(it)) if _looks_like_tsv(n)]
            if found:
                resolved += found
            else:
                print(f"  !! no .tsv files in folder: {os.path.abspath(it)}")
                stats["bad_files"] += 1
            continue
        if os.path.isfile(it):
            resolved.append(it)
            continue
        if os.path.isfile(it + ".txt"):  # "PSV_GAMES.tsv" saved as "PSV_GAMES.tsv.txt"
            resolved.append(it + ".txt")
            continue
        # Not found: show what IS there so the real name is obvious.
        stats["bad_files"] += 1
        hint_dir = os.path.dirname(it) or "."
        try:
            listing = os.listdir(hint_dir)
        except OSError:
            listing = []
        nearby = [n for n in listing if _looks_like_tsv(n) or "tsv" in n.lower()]
        print(f"  !! not found: {it}")
        if nearby:
            print(f"     but I see these in {os.path.abspath(hint_dir)} -- use these exact names:")
            for n in sorted(nearby):
                print(f"       {n}")
        else:
            print(f"     (no .tsv-looking files in {os.path.abspath(hint_dir)})")
    return resolved


def main():
    ap = argparse.ArgumentParser(description="Build a ux0/license folder from NoPayStation .tsv files.")
    ap.add_argument("tsv", nargs="*", default=["."],
                    help="one or more .tsv files, or a folder to scan (default: current folder)")
    ap.add_argument("--out", default="license", help="output license folder (default: ./license)")
    args = ap.parse_args()

    stats = {"written": 0, "no_zrif": 0, "errors": 0, "bad_files": 0}
    inputs = resolve_inputs(args.tsv or ["."], stats)
    for tsv in inputs:
        print(f"Processing {tsv} ...")
        process(tsv, args.out, stats)

    print("\n--- Summary ---")
    print(f"  .rif files written : {stats['written']}")
    print(f"  rows with no zRIF   : {stats['no_zrif']}  (free/no-DRM content -- no license needed)")
    print(f"  decode errors       : {stats['errors']}")
    if stats["bad_files"]:
        print(f"  unreadable files    : {stats['bad_files']}")
    print(f"\nLicenses are under: {os.path.abspath(args.out)}")
    print("Copy that folder into your  ux0/  as  ux0/license  (it merges with what's already there).")
    return 0 if stats["written"] else 1


if __name__ == "__main__":
    sys.exit(main())
