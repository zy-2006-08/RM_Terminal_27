#!/bin/bash
# 在仓库根目录生成两个开发用的 .app（空壳启动器，不打包程序）：
#   「RM 模拟器.app」= 双击 启动.command（终端里打开，选席位、看日志）
#   「RM 终端.app」  = 运行 build/macos/rm_terminal，配置/日志/录像都在 ~/Library/Application Support/rm_terminal
# 每次运行时按 .app 所在位置算仓库路径，所以仓库挪了、代码改了都不用重新生成（C++ 改了要重新编译）。
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

[ "$(uname -s)" = Darwin ] || { echo "[生成] 只能在 Mac 上运行" >&2; exit 1; }

APP_SIM="$ROOT/RM 模拟器.app"
APP_TERM="$ROOT/RM 终端.app"
ICON="$ROOT/resources/images/top_mid/red_team_logo.png"
VERSION=$(sed -nE 's/^project\(rm_terminal VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)
[ -n "$VERSION" ] || VERSION=0.0.0
MACOS=$(sw_vers -productVersion)

step() { echo "[生成] $*"; }
die() { echo "[生成] 出错：$*" >&2; exit 1; }

[ -x "$ROOT/build/macos/rm_terminal" ] || step "注意：还没有 build/macos/rm_terminal，先编译再双击「RM 终端」"

step "做图标"
ICONSET=$(mktemp -d)/AppIcon.iconset
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
  sips -z "$size" "$size" "$ICON" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
  sips -z $((size * 2)) $((size * 2)) "$ICON" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
ICNS="$ROOT/build/macos/AppIcon.icns"
mkdir -p "$ROOT/build/macos"
iconutil -c icns "$ICONSET" -o "$ICNS" || die "图标没做成"
rm -rf "$(dirname "$ICONSET")"

new_app() { # $1=目录 $2=显示名 $3=bundle id；壳子已建好，这里只写 Info.plist
  cat >"$1/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>zh_CN</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundleIdentifier</key><string>$3</string>
  <key>CFBundleName</key><string>$2</string>
  <key>CFBundleDisplayName</key><string>$2</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>${MACOS%%.*}.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSLocalNetworkUsageDescription</key><string>连接选手端电脑上的裁判系统、接收图传。</string>
</dict>
</plist>
EOF
  plutil -lint "$1/Contents/Info.plist" >/dev/null || die "$2 的 Info.plist 格式不对"
  cp "$ICNS" "$1/Contents/Resources/AppIcon.icns"
  chmod +x "$1/Contents/MacOS/launch"
  codesign --force --sign - "$1" 2>/dev/null || die "$2 签名失败"
  codesign --verify --strict "$1" || die "$2 签名检查没过"
}

step "生成「RM 模拟器.app」"
rm -rf "$APP_SIM"
mkdir -p "$APP_SIM/Contents/MacOS" "$APP_SIM/Contents/Resources"
cat >"$APP_SIM/Contents/MacOS/launch" <<'EOF'
#!/bin/bash
# 开发启动器：等于双击仓库里的 启动.command。
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
exec open -a Terminal "$REPO/启动.command"
EOF
new_app "$APP_SIM" "RM 模拟器" "com.robobraver.rm-terminal-sim"

step "生成「RM 终端.app」"
rm -rf "$APP_TERM"
mkdir -p "$APP_TERM/Contents/MacOS" "$APP_TERM/Contents/Resources"
cat >"$APP_TERM/Contents/MacOS/launch" <<'EOF'
#!/bin/bash
# 开发启动器：跑仓库里的最新编译产物，不打包。C++ 改了先 cmake --build build/macos --parallel。
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
DATA="$HOME/Library/Application Support/rm_terminal"
mkdir -p "$DATA"
[ -f "$DATA/rm_terminal.conf" ] || cp "$REPO/rm_terminal.conf.sample" "$DATA/rm_terminal.conf"
# 从程序坞启动不读 shell 配置，补上 Homebrew 的 ffmpeg 路径。
export PATH="/opt/homebrew/bin:/usr/local/bin:/opt/homebrew/sbin:$PATH"
cd "$DATA"
exec "$REPO/build/macos/rm_terminal" "$@"
EOF
new_app "$APP_TERM" "RM 终端" "com.robobraver.rm-terminal"

step "好了："
step "  $APP_SIM"
step "  $APP_TERM"
step "  拖到程序坞就能用；仓库挪位置也不用重新生成。"
