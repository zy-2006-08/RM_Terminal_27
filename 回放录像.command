#!/bin/bash
# 双击弹出选择框挑一份录像；把某个 .rmrec 拖到本文件上则直接回放那一份。
cd "$(dirname "$0")" || exit 1

BINARY=build/macos/rm_terminal
DIR="$HOME/Library/Application Support/rm_terminal/recordings"

if [ ! -x "$BINARY" ]; then
  echo "没找到 $BINARY，请先构建。"
  read -r -p "回车退出" _
  exit 1
fi

REC="$1"
if [ -z "$REC" ]; then
  if [ ! -d "$DIR" ] || ! ls "$DIR"/*.rmrec >/dev/null 2>&1; then
    echo "还没有录像：$DIR"
    read -r -p "回车退出" _
    exit 1
  fi
  REC=$(osascript 2>/dev/null <<EOF
set theFile to choose file with prompt "选择要回放的录像" default location POSIX file "$DIR"
POSIX path of theFile
EOF
)
fi

[ -n "$REC" ] || exit 0
case "$REC" in
  *.rmrec) ;;
  *) echo "请选择 .rmrec 录像：$REC"; read -r -p "回车退出" _; exit 1 ;;
esac

echo "[回放] $REC"
"$BINARY" --replay "$REC" &
CHILD=$!
trap 'kill "$CHILD" 2>/dev/null; pkill -P "$CHILD" 2>/dev/null' EXIT HUP INT TERM
wait "$CHILD"
