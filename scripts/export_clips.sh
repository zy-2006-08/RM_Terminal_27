#!/usr/bin/env bash
# 把一份复盘报告里每件事的片段（录像里前 10 秒到后 5 秒）导成 MP4，放在报告旁边的 clips/ 里，
# 报告网页里的「MP4」链接就能点了。
#
#   bash scripts/export_clips.sh ~/.local/share/rm_terminal/reports/2026-11-16_14-35-42_第1局.json
#
# 要装 ffmpeg。每段按原速离屏放一遍再编码，15 秒的片段大约 20 秒。
# 分辨率默认 1440x900，RM_TERMINAL_CAPTURE_SIZE=1920x1080 可以改。
set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$(uname -s)" in
    Darwin) BUILD_DIR=build/macos ;;
    *)      BUILD_DIR=build/ubuntu ;;
esac
BIN="${RM_TERMINAL_BINARY:-$REPO/$BUILD_DIR/rm_terminal}"
fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }

[ $# -eq 1 ] || fail "用法: bash scripts/export_clips.sh <复盘报告的 .json>"
JSON="$1"
[ -f "$JSON" ] || fail "找不到 $JSON"
[ -x "$BIN" ] || fail "terminal binary not found: $BIN"
command -v ffmpeg > /dev/null || fail "要先装 ffmpeg"
DIR="$(cd "$(dirname "$JSON")" && pwd)"
mkdir -p "$DIR/clips"

made=0
while IFS=$'\t' read -r recording from to mp4 text; do
    [ -f "$recording" ] || fail "录像不在了: $recording"
    printf '%s  %s-%s 秒  %s\n' "$mp4" "$from" "$to" "$text"
    QT_QPA_PLATFORM=offscreen RM_TERMINAL_CAPTURE_SIZE="${RM_TERMINAL_CAPTURE_SIZE:-1440x900}" \
        "$BIN" --no-record --replay "$recording" --from "$from" --to "$to" --export-video "$DIR/$mp4" \
        < /dev/null > "$DIR/clips/export.log" 2>&1 || fail "导出失败，看 $DIR/clips/export.log"
    made=$((made + 1))
done < <(python3 - "$JSON" <<'PY'
import json, sys
report = json.load(open(sys.argv[1], encoding="utf-8"))
for clip in report.get("clips", []):
    print("\t".join([report["recording"], f"{clip['from']:.0f}", f"{clip['to']:.0f}", clip["mp4"],
                     clip["text"].replace("\t", " ")]))
PY
)
rm -f "$DIR/clips/export.log"
echo "导出了 $made 段 -> $DIR/clips/"
