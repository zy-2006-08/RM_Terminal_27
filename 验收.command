#!/bin/bash
# 双击验收:跑赛程 + 机器人控制与 MQTT 回读断言。窗口停留,看清 PASS/FAIL。
cd "$(dirname "$0")" || exit 1
./scripts/accept_sim_control.sh
echo
read -n 1 -s -r -p "按任意键关闭窗口"
