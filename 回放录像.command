#!/bin/bash
# 双击弹出选择框挑一份录像；把某个 .rmrec 拖到本文件上则直接回放那一份。
# Linux 上没有 osascript：用 `bash 回放录像.command` 运行，按编号选录像。
cd "$(dirname "$0")" || exit 1

case "$(uname -s)" in
  Darwin)
    BINARY=build/macos/rm_terminal
    DIR="$HOME/Library/Application Support/rm_terminal/recordings"
    ;;
  *)
    BINARY=build/ubuntu/rm_terminal
    DIR="${XDG_DATA_HOME:-$HOME/.local/share}/rm_terminal/recordings"
    ;;
esac

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
  if [ "$(uname -s)" = Darwin ]; then
    REC=$(osascript 2>/dev/null <<EOF
set theFile to choose file with prompt "选择要回放的录像" default location POSIX file "$DIR"
POSIX path of theFile
EOF
)
  else
    # 最新的排在最前；文件名是时间戳，不含空格。
    mapfile -t FILES < <(ls -1t "$DIR"/*.rmrec)
    echo "选择要回放的录像（最新在前）："
    for i in "${!FILES[@]}"; do
      printf '  %2d) %s\n' "$((i + 1))" "$(basename "${FILES[$i]}")"
    done
    read -r -p "输入编号（直接回车取消）: " pick
    [ -n "$pick" ] || exit 0
    if ! [[ "$pick" =~ ^[0-9]+$ ]] || [ "$pick" -lt 1 ] || [ "$pick" -gt "${#FILES[@]}" ]; then
      echo "无效编号：$pick"
      exit 1
    fi
    REC="${FILES[$((pick - 1))]}"
  fi
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
