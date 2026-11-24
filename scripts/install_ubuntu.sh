#!/bin/bash
# Ubuntu 一键安装：新电脑 clone 下来以后运行一次，桌面上就有「RM 模拟器」「RM 真机」「RM 回放」三个图标。
#
#   bash scripts/install_ubuntu.sh
#
# 按顺序做：检查系统软件 -> 建 .venv（模拟器用）-> 编译终端 -> 装启动脚本 -> 装桌面图标
# -> 放一份真机配置 -> 问要不要装离线语音。已经装好的步骤会跳过，重复运行没关系。
# 只往仓库里的 .venv、build/ubuntu 和你的主文件夹里写，不用管理员权限；缺系统软件时只打印安装命令。
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
step() { echo "[安装] $*"; }

if [ "$(id -u)" -eq 0 ]; then
  echo "别用 sudo 运行：图标和配置会装进 root 的主文件夹。直接运行 bash scripts/install_ubuntu.sh"
  exit 1
fi

# 1. 系统软件：和 BUILD.md 的 Ubuntu 一节一致，另加 zenity（图标的弹窗用它）。
PACKAGES=(build-essential cmake
  qt6-base-dev qt6-declarative-dev qt6-svg-dev
  qml6-module-qtquick qml6-module-qtquick-layouts
  libprotobuf-dev protobuf-compiler libabsl-dev
  libmosquitto-dev mosquitto ffmpeg
  python3-venv python3-tk fonts-noto-cjk zenity)
missing=()
for pkg in "${PACKAGES[@]}"; do
  dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed' || missing+=("$pkg")
done
if [ "${#missing[@]}" -gt 0 ]; then
  echo "缺这些系统软件。先运行下面这条（要输管理员密码），装完再运行一次本脚本："
  echo
  echo "  sudo apt-get install ${missing[*]}"
  exit 1
fi
step "1/6 系统软件齐了"

# 2. 模拟器用的 Python 环境，和 README、run.sh 提示的是同一条命令。
if .venv/bin/python -c 'import google.protobuf, paho.mqtt' 2>/dev/null; then
  step "2/6 .venv 已经装好，跳过"
else
  step "2/6 建 .venv，装 protobuf 和 paho-mqtt（模拟器用）"
  [ -x .venv/bin/python ] || python3 -m venv .venv
  .venv/bin/pip install --disable-pip-version-check protobuf paho-mqtt
fi

# 3. 编译终端。
if [ -x build/ubuntu/rm_terminal ]; then
  step "3/6 终端已经编译过，跳过（改了代码要重新编译：cmake --build build/ubuntu --parallel）"
else
  step "3/6 编译终端，第一次要几分钟"
  cmake -S . -B build/ubuntu -DCMAKE_BUILD_TYPE=Release
  # 同时编译的数量按 CPU 核数限定。不给数字的 --parallel 对 make 是不限数量，内存小的电脑会被撑爆。
  cmake --build build/ubuntu --parallel "$(nproc)"
fi

# 4. 启动脚本：~/.local/bin 里放一个指向仓库那份的快捷方式，git pull 拿到新版就生效。
BIN="$HOME/.local/bin"
LAUNCHER="$BIN/rm-terminal-launch"
mkdir -p "$BIN"
if [ -e "$LAUNCHER" ] && [ ! -L "$LAUNCHER" ]; then
  mkdir -p "$HOME/.local/share/rm_terminal"
  mv "$LAUNCHER" "$HOME/.local/share/rm_terminal/rm-terminal-launch.old"
  step "原来的启动脚本挪到 ~/.local/share/rm_terminal/rm-terminal-launch.old 留着"
fi
ln -sfn "$REPO/scripts/ubuntu/rm-terminal-launch" "$LAUNCHER"
step "4/6 启动脚本：$LAUNCHER"

# 5. 桌面图标，应用列表和桌面各一份，路径按这台电脑的实际位置填。
APPS="$HOME/.local/share/applications"
DESKTOP="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
if [ -z "$DESKTOP" ] || [ "$DESKTOP" = "$HOME" ]; then
  DESKTOP="$HOME/Desktop"
fi
mkdir -p "$APPS" "$DESKTOP"

write_entry() {  # 文件 名字 说明 启动参数 图标
  cat >"$1" <<EOF
[Desktop Entry]
Type=Application
Name=$2
Comment=$3
Exec=$LAUNCHER $4
Icon=$5
Terminal=true
Categories=Development;
EOF
}
for dir in "$APPS" "$DESKTOP"; do
  write_entry "$dir/rm-terminal.desktop" "RM 模拟器" "启动模拟器、自定义终端和控制面板" start \
    "$REPO/resources/images/top_mid/red_team_logo.png"
  write_entry "$dir/rm-terminal-real.desktop" "RM 真机" "连真实裁判系统（选手端电脑），不启动模拟器" real \
    "$REPO/scripts/ubuntu/rm-terminal-real.png"
  write_entry "$dir/rm-terminal-replay.desktop" "RM 回放" "回放比赛录像" replay \
    "$REPO/resources/images/top_mid/blue_team_logo.png"
done
# 桌面上的图标要能运行、并标成「信任」，双击才会启动，不然会被当成文本文件打开。
for name in rm-terminal rm-terminal-real rm-terminal-replay; do
  chmod +x "$DESKTOP/$name.desktop"
  gio set "$DESKTOP/$name.desktop" metadata::trusted true 2>/dev/null \
    || step "（$name.desktop 标不了「信任」：第一次双击时选「允许启动」就行）"
done
step "5/6 图标：$DESKTOP 和应用列表里各三个"

# 6. 真机配置：每台电脑一份，在主文件夹里。已经有了就不动。
REAL_CONF="$HOME/rm_terminal.26engine.conf"
if [ -f "$REAL_CONF" ]; then
  step "6/6 真机配置已经有了，不动：$REAL_CONF"
else
  cp scripts/ubuntu/rm_terminal.26engine.conf "$REAL_CONF"
  step "6/6 放好真机配置：$REAL_CONF（服务器地址、串口要改就改这份）"
fi

# 离线语音（可选）：要联网下载，装好约占 265MB，所以先问一句。
TTS_DIR="${RM_TERMINAL_TTS_DIR:-$HOME/.local/share/rm_terminal/tts}"
if [ -x "$TTS_DIR/venv/bin/python" ] && [ -f "$TTS_DIR/voices/zh_CN-chaowen-medium.onnx" ]; then
  step "离线语音已经装好，跳过"
else
  answer=""
  read -r -p "[安装] 要装离线中文语音吗？要联网下载，装好约占 265MB；不装的话提醒只有横幅没有声音 [y/N] " answer || true
  case "$answer" in
    y | Y | yes) bash scripts/setup_voice.sh ;;
    *) step "先不装语音。以后要装：bash scripts/setup_voice.sh" ;;
  esac
fi

echo
step "装好了。桌面上「RM 模拟器」用来练习，「RM 真机」连官方裁判系统，「RM 回放」看录像。"
