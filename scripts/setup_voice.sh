#!/bin/bash
# 安装 Linux 上的离线中文语音（Piper + 超文）。装好后终端的提醒才有声音；
# macOS 用系统自带的语音，不需要这一步。
#
#   bash scripts/setup_voice.sh
#
# 装到 ~/.local/share/rm_terminal/tts（可用 RM_TERMINAL_TTS_DIR 改），不进仓库。
# 需要联网一次：Python 包约 60 MB，声音模型约 63 MB。模型先从 Hugging Face 官方地址
# 下载，失败再换镜像，两种来源都按官方公布的 SHA-256 核对，对不上就删掉报错。
set -euo pipefail

DIR="${RM_TERMINAL_TTS_DIR:-$HOME/.local/share/rm_terminal/tts}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VOICE=zh_CN-chaowen-medium
MODEL_SHA256=820d64ac16048fbcf38dd0823d37fab5f5e0c2bd71b01ca5a50f553fac19e746
CONFIG_SHA256=a6bb2caafa0645642f13cbf7e2f6fbbb16fded66e51109fc26d622f6472fa16f
REMOTE=rhasspy/piper-voices/resolve/main/zh/zh_CN/chaowen/medium

command -v python3 >/dev/null || { echo "找不到 python3"; exit 1; }
python3 -c 'import venv' 2>/dev/null || { echo "缺 python3-venv：sudo apt-get install python3-venv"; exit 1; }
if ! command -v pw-play >/dev/null && ! command -v paplay >/dev/null && ! command -v aplay >/dev/null; then
  echo "找不到播放程序（pw-play / paplay / aplay），装好音频系统后再试"; exit 1
fi

echo "[voice] 1/3 Python 环境 -> $DIR/venv"
mkdir -p "$DIR/voices"
[ -x "$DIR/venv/bin/python" ] || python3 -m venv "$DIR/venv"
"$DIR/venv/bin/pip" install --quiet --disable-pip-version-check "piper-tts==1.8.0" "pypinyin==0.55.0"

fetch() {
  local file=$1 sha=$2 out="$DIR/voices/$1"
  if [ -s "$out" ] && [ "$(sha256sum "$out" | cut -d' ' -f1)" = "$sha" ]; then
    echo "[voice]     $file 已存在，校验通过"; return
  fi
  for base in https://huggingface.co https://hf-mirror.com; do
    rm -f "$out.part"
    if curl -sSfL --retry 2 --max-time 900 -o "$out.part" "$base/$REMOTE/$file"; then
      if [ "$(sha256sum "$out.part" | cut -d' ' -f1)" = "$sha" ]; then
        mv "$out.part" "$out"; echo "[voice]     $file 下载完成（$base），校验通过"; return
      fi
      echo "[voice]     $file 从 $base 下载的内容校验不通过，丢弃"
    fi
  done
  rm -f "$out.part"; echo "[voice] 下载 $file 失败"; exit 1
}

echo "[voice] 2/3 声音「超文」"
fetch "$VOICE.onnx" "$MODEL_SHA256"
fetch "$VOICE.onnx.json" "$CONFIG_SHA256"

echo "[voice] 3/3 试合成一句"
TEST="$(mktemp --suffix=.wav)"
"$DIR/venv/bin/python" "$REPO/scripts/tts_say.py" --model "$DIR/voices/$VOICE.onnx" \
  --output "$TEST" -- 语音安装完成
[ -s "$TEST" ] || { echo "[voice] 试合成失败"; exit 1; }
rm -f "$TEST"
echo "[voice] 装好了。重启终端后，提醒就有声音了。"
