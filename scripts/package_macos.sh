#!/bin/bash
# 在 Mac 上把终端打成双击就能用的「RM 终端.app」，装进「应用程序」，再出一个 .dmg 给别的 Mac。
#
#   bash scripts/package_macos.sh
#
# 打出来的程序连真实裁判系统（选手端电脑 192.168.12.1），打开先弹选席位页；模拟器还是用 启动.command。
# 第一次要联网：Homebrew 装编译用的库，再下载 ffmpeg 源码现编一个只带 H.265 解码的精简版
# （几 MB，不依赖 Homebrew，放进程序包里）。再运行一次就是升级：只重编改过的部分，覆盖装好的那个。
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

APP_NAME="RM 终端"
OUT=build/macos-app
# 绝对路径：下面的自检要换到 / 和临时目录里去启动程序。
APP="$ROOT/$OUT/$APP_NAME.app"
FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e

step() { echo "[打包] $*"; }
die() { echo "[打包] 出错：$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "这个脚本只能在 Mac 上运行"
ARCH=$(uname -m)
MACOS=$(sw_vers -productVersion)
if [ "$ARCH" != arm64 ] || [ "${MACOS%%.*}" -lt 15 ]; then
  step "这台 Mac 是 ${ARCH}、macOS ${MACOS}。Homebrew 只给 Apple 芯片、macOS 15 及以上提供编好的 Qt，"
  step "这台机器上要从源码编 Qt，可能要好几个小时。"
  read -r -p "[打包] 仍要继续请按回车，不继续按 Ctrl+C：" _
fi

if ! xcode-select -p >/dev/null 2>&1; then
  xcode-select --install || true
  die "先装好弹窗里的「命令行开发者工具」，装完再运行一次"
fi
if ! command -v brew >/dev/null 2>&1; then
  for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
    if [ -x "$candidate" ]; then eval "$("$candidate" shellenv)"; break; fi
  done
fi
command -v brew >/dev/null 2>&1 || die "没装 Homebrew：打开 https://brew.sh ，照首页那一行命令装好，再运行一次"

step "1/6 用 Homebrew 装编译用的库（装过的会跳过）"
brew install cmake qtbase qtdeclarative qtsvg protobuf abseil mosquitto

step "2/6 编译终端（第一次要几分钟）"
cmake -S . -B "$OUT" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$OUT" --parallel --target rm_terminal

FF_DIR="$OUT/ffmpeg-$FFMPEG_VERSION"
if [ ! -x "$FF_DIR/ffmpeg" ]; then
  step "3/6 编精简版 ffmpeg ${FFMPEG_VERSION}（第一次约 1 分钟）"
  TARBALL="$OUT/ffmpeg-$FFMPEG_VERSION.tar.xz"
  [ -f "$TARBALL" ] || curl -fL --retry 3 -o "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
  if ! echo "$FFMPEG_SHA256  $TARBALL" | shasum -a 256 -c - >/dev/null; then
    rm -f "$TARBALL"
    die "下载的 ffmpeg 源码校验不对，已经删掉，再运行一次"
  fi
  rm -rf "$FF_DIR"
  tar -xf "$TARBALL" -C "$OUT"
  # 只编 cpp/video_receiver.cpp 调 ffmpeg 用到的部分：管道进、H.265 解码、缩放、BGR24 裸帧出。
  # --disable-autodetect：不去链本机找到的任何库，编出来的 ffmpeg 拷到别的 Mac 上也能跑。
  FF_FLAGS=(--disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug
    --disable-programs --enable-ffmpeg --enable-protocol=pipe --enable-demuxer=hevc --enable-parser=hevc
    --enable-decoder=hevc --enable-filter=scale --enable-encoder=rawvideo --enable-muxer=rawvideo)
  if [ "$ARCH" = x86_64 ] && ! command -v nasm >/dev/null 2>&1; then FF_FLAGS+=(--disable-x86asm); fi
  if ! (cd "$FF_DIR" && ./configure "${FF_FLAGS[@]}" >configure.out 2>&1 \
        && make -j"$(sysctl -n hw.ncpu)" >make.out 2>&1); then
    die "ffmpeg 没编过，看 $FF_DIR/configure.out 和 $FF_DIR/make.out"
  fi
else
  step "3/6 精简版 ffmpeg 已经编过，跳过"
fi

step "4/6 装配程序包"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$OUT/rm_terminal" "$APP/Contents/MacOS/rm_terminal"
cp "$FF_DIR/ffmpeg" "$APP/Contents/MacOS/ffmpeg"
# 第一次双击打开时，终端把这份配置复制到 ~/Library/Application Support/rm_terminal/（cpp/main.cpp）。
cat >"$APP/Contents/Resources/rm_terminal.conf" <<'EOF'
# RM 终端（Mac）的配置。第一次双击打开时从程序包里复制过来，之后改这个文件，不用重新打包。
# 每一项的意思见项目里的 rm_terminal.conf.sample 和 BUILD.md 的 Configuration 一节。
#
# 连真实裁判系统：官方选手端电脑接终端的那个网口是 192.168.12.1。
# 这台 Mac 接选手端的网口要手动设成 192.168.12.2，子网掩码 255.255.255.0，路由器（网关）空着。
mqtt_host=192.168.12.1
mqtt_port=3333
udp_port=3334
# 席位不写：每次打开先弹选席位页。
# 接自定义控制器时，去掉下一行开头的 #，路径换成 ls /dev/cu.* 查到的那个：
# controller_serial=/dev/cu.usbserial-XXXX
EOF
VERSION=$(sed -nE 's/^project\(rm_terminal VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)
# 用 Homebrew 的库编出来的程序只能在同版本或更新的 macOS 上运行，所以最低版本写这台机器的。
cat >"$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>zh_CN</string>
  <key>CFBundleExecutable</key><string>rm_terminal</string>
  <key>CFBundleIdentifier</key><string>com.robobraver.rm-terminal</string>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>${MACOS%%.*}.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSLocalNetworkUsageDescription</key><string>连接选手端电脑上的裁判系统、接收图传。</string>
</dict>
</plist>
EOF
plutil -lint "$APP/Contents/Info.plist" >/dev/null || die "Info.plist 格式不对"
ICONSET="$OUT/AppIcon.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
  sips -z "$size" "$size" resources/images/top_mid/red_team_logo.png --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
  sips -z $((size * 2)) $((size * 2)) resources/images/top_mid/red_team_logo.png \
    --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" || step "图标没做成，程序用默认图标，不影响使用"

step "5/6 把 Qt 和其他库放进程序包"
MACDEPLOYQT=$(command -v macdeployqt || true)
if [ -z "$MACDEPLOYQT" ]; then
  MACDEPLOYQT=$(find "$(brew --prefix qtbase)" -name macdeployqt -type f -perm -u+x 2>/dev/null | head -n 1 || true)
fi
[ -n "$MACDEPLOYQT" ] || die "找不到 macdeployqt（应该在 Homebrew 的 qtbase 里）"
# -qmldir：界面里的 QML 要 QtQuick 和 QtQuick.Layouts，不扫这个目录就不会打进包里。
# 自己注册的 RmTerminal.Glass 它找不到会报一句，不用管；包能不能用看第 6 步的检查。
"$MACDEPLOYQT" "$APP" -qmldir=qml -always-overwrite || step "macdeployqt 报了错，第 6 步的检查会确认程序包能不能用"

step "6/6 签名、检查"
# Apple 芯片上每个程序和库都要有签名才能加载；macdeployqt 改过的库签名已经失效，从里到外重签一遍。
: >"$OUT/codesign.log"
while IFS= read -r -d '' file; do
  codesign --force --sign - "$file" 2>>"$OUT/codesign.log" || die "签名失败：${file}（看 $OUT/codesign.log）"
done < <(find "$APP/Contents" -type f \( -name '*.dylib' -o -perm -u+x \) ! -path '*.framework/*' -print0)
while IFS= read -r -d '' framework; do
  codesign --force --sign - "$framework" 2>>"$OUT/codesign.log" || die "签名失败：${framework}（看 $OUT/codesign.log）"
done < <(find "$APP/Contents" -type d -name '*.framework' -prune -print0)
codesign --force --sign - "$APP" 2>>"$OUT/codesign.log" || die "签名失败（看 $OUT/codesign.log）"
codesign --verify --deep --strict "$APP" || die "签名检查没过"

# 只看「会被加载」的依赖（LC_LOAD_DYLIB 等）：LC_ID_DYLIB 是库自己的名字，写的是 Homebrew 路径也不碍事，
# macdeployqt 不会改它，otool -L 把它列在第一行，容易误报。
LEAKS=$(find "$APP/Contents" -type f -print0 | xargs -0 otool -l 2>/dev/null \
  | awk '/^[^\t ]/ { file = $0 }
         /^ +cmd / { cmd = $2 }
         /^ +name \/(opt\/homebrew|usr\/local)\// {
           if (cmd == "LC_LOAD_DYLIB" || cmd == "LC_REEXPORT_DYLIB" || cmd == "LC_LOAD_WEAK_DYLIB") print file " -> " $2
         }' || true)
[ -z "$LEAKS" ] || die "程序包里还有库指向 Homebrew，拿到别的 Mac 上会打不开：
$LEAKS"

FRAME_BYTES=$((640 * 360 * 3))
DECODED=$("$APP/Contents/MacOS/ffmpeg" -hide_banner -loglevel quiet -hwaccel none -fflags nobuffer -flags low_delay \
  -threads 2 -probesize 262144 -analyzeduration 2000000 -f hevc -c:v hevc -i pipe:0 -vf scale=640:360 -threads 2 \
  -fps_mode passthrough -frames:v 60 -f rawvideo -pix_fmt bgr24 pipe:1 <sim/assets/sim_feed.h265 | wc -c || true)
[ $((DECODED / FRAME_BYTES)) -gt 0 ] || die "包里的 ffmpeg 解不出测试图传 sim/assets/sim_feed.h265"

# 像从访达双击那样（工作目录 /）启动一次不开窗口的自检：要能读到配置、写得进日志。
(cd / && "$APP/Contents/MacOS/rm_terminal" --safe-smoke >/dev/null 2>&1) \
  || die "像双击那样启动的自检没过，看 ~/Library/Application Support/rm_terminal/rm_terminal.log"

# 真把主界面画一次（会闪一下窗口），确认 QML 界面要的模块都打进包里了。连的是本机一个没人用的端口，不碰真网络。
SMOKE=$(mktemp -d)
printf 'mqtt_host=127.0.0.1\nmqtt_port=1\n' >"$SMOKE/smoke.conf"
if ! (cd "$SMOKE" && "$APP/Contents/MacOS/rm_terminal" --config smoke.conf --seat 3 --no-record \
      --screenshot shot.png >out.txt 2>&1); then
  cat "$SMOKE/out.txt" >&2
  die "主界面没画出来"
fi
if grep -E 'is not installed|Could not find the Qt platform plugin|Library not loaded' "$SMOKE/out.txt" >&2 \
   || [ ! -s "$SMOKE/shot.png" ]; then
  die "主界面缺东西（上面是报错）"
fi
rm -rf "$SMOKE"

DEST="/Applications/$APP_NAME.app"
if [ ! -w /Applications ]; then
  mkdir -p "$HOME/Applications"
  DEST="$HOME/Applications/$APP_NAME.app"
fi
rm -rf "$DEST"
ditto "$APP" "$DEST"

# 「RM 模拟器」：单包一层 .app，双击等于双击项目里的启动.command，这样启动台里有图标可点。
# 里面记的是打包时这个项目文件夹的绝对路径，挪了文件夹就重新打包一次。
APP_NAME_SIM="RM 模拟器"
SIM="$(dirname "$DEST")/$APP_NAME_SIM.app"
rm -rf "$SIM"
mkdir -p "$SIM/Contents/MacOS" "$SIM/Contents/Resources"
cat >"$SIM/Contents/MacOS/launch" <<EOF
#!/bin/bash
exec open -a Terminal "$ROOT/启动.command"
EOF
chmod +x "$SIM/Contents/MacOS/launch"
cp "$APP/Contents/Resources/AppIcon.icns" "$SIM/Contents/Resources/" 2>/dev/null || true
cat >"$SIM/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>zh_CN</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundleIdentifier</key><string>com.robobraver.rm-terminal-sim</string>
  <key>CFBundleName</key><string>$APP_NAME_SIM</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME_SIM</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>${MACOS%%.*}.0</string>
</dict>
</plist>
EOF
plutil -lint "$SIM/Contents/Info.plist" >/dev/null || die "「$APP_NAME_SIM」的 Info.plist 格式不对"
codesign --force --sign - "$SIM" 2>>"$OUT/codesign.log" || die "「$APP_NAME_SIM」签名失败（看 $OUT/codesign.log）"

DMG="$OUT/RM-Terminal-macOS-$ARCH.dmg"
STAGE="$OUT/dmg"
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/$APP_NAME.app"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "$APP_NAME" -srcfolder "$STAGE" -format UDZO "$DMG"
rm -rf "$STAGE"

step "好了。"
step "  程序：${DEST}（启动台里也有，双击就开；可以拖到程序坞上）"
step "  第一次打开会问能不能访问本地网络，点「允许」，不然连不上裁判系统。"
step "  配置：~/Library/Application Support/rm_terminal/rm_terminal.conf（第一次打开以后才有），日志和录像也在这个文件夹"
step "  给别的 Mac：${DMG}（要 Apple 芯片、macOS ${MACOS%%.*} 或更新；第一次打开被拦时，去「系统设置 → 隐私与安全性」点「仍要打开」）"
open -R "$DEST"
