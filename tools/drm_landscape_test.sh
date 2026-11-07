#!/bin/sh
# Build the landscape-strip harness: extract the code under test from drm.c, cross-compile static armv7
# (CC=cc for a host run). Usage: drm_landscape_test.sh [outdir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
drm="$here/../lv_drivers/display/drm.c"
out=${1:-.}
CC=${CC:-armv7-linux-gnueabihf-gcc}
awk '/^void drm_wf_present\(const/{p=1} /^uint32_t drm_frame_ms/{p=0} p' "$drm" > "$out/drm_extract.inc"
grep -q 'copy_cols' "$out/drm_extract.inc" && grep -q 'BOARD_FB_ROTATE_90' "$out/drm_extract.inc"
$CC -O2 -static -DBOARD_FB_ROTATE_90=0 -I"$out" -o "$out/drm_landscape_test" "$here/drm_landscape_test.c"
echo "built $out/drm_landscape_test"
