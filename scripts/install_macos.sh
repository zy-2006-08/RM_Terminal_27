#!/bin/bash
# Mac 一键安装：新电脑 clone 下来以后运行一次，装依赖 -> 建 .venv -> 编译 -> 生成两个启动器。
#
#   bash scripts/install_macos.sh
#
# 对应 scripts/install_ubuntu.sh。已装好的步骤会跳过，重复运行没关系。
# 命令行开发者工具和 Homebrew 要输密码，不自动装，缺了就打印命令。
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
step() { echo "[安装] $*"; }
die() { echo "[安装] 出错：$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "这个脚本只能在 Mac 上运行；Ubuntu 用 bash scripts/install_ubuntu.sh"

# 1. 命令行开发者工具 + Homebrew。
if ! xcode-select -p >/dev/null 2>&1; then
  xcode-select --install || true
  die "先装好弹窗里的「命令行开发者工具」，装完再运行一次本脚本"
fi
if ! command -v brew >/dev/null 2>&1; then
  for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
    if [ -x "$candidate" ]; then eval "$("$candidate" shellenv)"; break; fi
  done
fi
if ! command -v brew >/dev/null 2>&1; then
  echo "没装 Homebrew。先运行这条（要输密码），装完再运行本脚本："
  echo
  echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
  exit 1
fi

ARCH=$(uname -m)
MACOS=$(sw_vers -productVersion)
if [ "$ARCH" != arm64 ] || [ "${MACOS%%.*}" -lt 15 ]; then
  step "这台是 ${ARCH}、macOS ${MACOS}。Homebrew 只给 Apple 芯片、macOS 15 及以上提供编好的 Qt，"
  step "这台机器上装 Qt 可能要源码编几个小时。"
  if [ -z "${RM_INSTALL_YES:-}" ]; then
    read -r -p "[安装] 仍要继续请按回车，不继续按 Ctrl+C：" _ || true
  fi
fi

# 和 scripts/package_macos.sh 同一份依赖；ffmpeg 用 Homebrew 版（开发机不需要自带）。
step "1/5 装依赖（装过的会跳过）"
brew install cmake qtbase qtdeclarative qtsvg protobuf abseil mosquitto ffmpeg

# 2. 模拟器用的 Python 环境，和 README、run.sh 提示的是同一条命令。
if .venv/bin/python -c 'import google.protobuf, paho.mqtt' 2>/dev/null; then
  step "2/5 .venv 已经装好，跳过"
else
  step "2/5 建 .venv，装 protobuf 和 paho-mqtt（模拟器用）"
  [ -x .venv/bin/python ] || python3 -m venv .venv
  .venv/bin/pip install --disable-pip-version-check protobuf paho-mqtt
fi

# 3. 编译终端。
if [ -x build/macos/rm_terminal ]; then
  step "3/5 终端已经编译过，跳过（改了代码要重新编译：cmake --build build/macos --parallel）"
else
  step "3/5 编译终端，第一次要几分钟"
  cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release
  # 按 CPU 核数限并发：不给数字的 --parallel 对 make 是不限数量，内存小的电脑会被撑爆。
  cmake --build build/macos --parallel "$(sysctl -n hw.ncpu)"
fi

# 4. 用刚编好的程序自检一次（不开窗口），确认 Qt 插件和库都找得到。
step "4/5 快速自检"
SMOKE=$(mktemp -d)
printf 'mqtt_host=127.0.0.1\nmqtt_port=1\n' >"$SMOKE/smoke.conf"
(cd "$SMOKE" && "$REPO/build/macos/rm_terminal" --config smoke.conf --safe-smoke >/dev/null) \
  || die "自检没过，看上面的报错"
rm -rf "$SMOKE"

# 5. 仓库根目录的两个 .app 空壳，拖进程序坞即可。
step "5/5 生成「RM 模拟器.app」和「RM 终端.app」"
bash scripts/make_mac_launchers.sh

if ! .venv/bin/python -c 'import tkinter' >/dev/null 2>&1; then
  TK=$(.venv/bin/python -c 'import sys; print(f"python-tk@{sys.version_info.major}.{sys.version_info.minor}")' 2>/dev/null || echo python-tk)
  step "控制面板要 Tkinter，现在没有：brew install $TK 后重开模拟器（不装也能跑，只是没控制面板）"
fi

echo
step "装好了。把仓库根目录的「RM 模拟器.app」「RM 终端.app」拖进程序坞，双击就能用。"
step "改了 C++ 重新编译：cmake --build build/macos --parallel"
