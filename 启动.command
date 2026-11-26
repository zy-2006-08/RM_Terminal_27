#!/bin/bash
# 双击启动：先手动选自机，再起 mosquitto + 模拟器 + 图传 + 终端 + 控制面板。关掉窗口即全部停止。
cd "$(dirname "$0")" || exit 1

# 席位必须每次启动手动选择，没有默认值；RM_SELF_ID 已显式给出时视为已选，
# 供验收脚本和 CI 使用。选定后进程启动即锁定，比赛中不能改。模拟器也要知道席位，
# 所以在这里选，终端看到 RM_SELF_ID 就不会再弹选席位页。
if [ -z "$RM_SELF_ID" ]; then
  echo "请选择本次比赛的席位："
  echo "   1) 红方 1 号 重装       2) 红方 3 号 步兵"
  echo "   3) 红方 4 号 步兵       4) 红方 6 号 指挥手"
  echo "   5) 蓝方 1 号 重装       6) 蓝方 3 号 步兵"
  echo "   7) 蓝方 4 号 步兵       8) 蓝方 6 号 指挥手"
  while :; do
    read -r -p "输入编号 1-8（回车确认）: " pick
    case "$pick" in
      1) RM_SELF_ID=1 ;;   2) RM_SELF_ID=3 ;;
      3) RM_SELF_ID=4 ;;   4) RM_SELF_ID=6 ;;
      5) RM_SELF_ID=101 ;; 6) RM_SELF_ID=103 ;;
      7) RM_SELF_ID=104 ;; 8) RM_SELF_ID=106 ;;
      *) echo "无效编号：$pick" ; continue ;;
    esac
    break
  done
fi

case "$RM_SELF_ID" in
  1|3|4|6|101|103|104|106) ;;
  *) echo "无效的 RM_SELF_ID=$RM_SELF_ID（只有 1、3、4、6、101、103、104、106）" ; exit 1 ;;
esac
export RM_SELF_ID
echo "[启动] 席位: $RM_SELF_ID"

# 控制面板是 Tk 写的；Homebrew 的 Python 默认不带 Tkinter。缺了就跳过面板，
# 打印一行怎么装，而不是甩一屏 traceback。终端本体不受影响。
if .venv/bin/python -c "import tkinter" >/dev/null 2>&1; then
  .venv/bin/python sim/control_panel.py &
  PANEL=$!
  trap 'kill "$PANEL" 2>/dev/null' EXIT
else
  TK_PKG=$(.venv/bin/python -c 'import sys; print(f"python-tk@{sys.version_info.major}.{sys.version_info.minor}")' 2>/dev/null || echo python-tk)
  echo "[启动] 没有控制面板需要的 Tkinter，本次跳过面板（终端本体照常启动）。"
  if [ "$(uname -s)" = Darwin ]; then
    echo "[启动] 要控制面板就关掉窗口，运行 brew install $TK_PKG 再重新启动。"
  else
    echo "[启动] 要控制面板就关掉窗口，运行 sudo apt install python3-tk 再重新启动。"
  fi
fi
./run.sh
