# role-layouts · 选席位与分角色布局 施工计划

## 一句话说明（给人看）

开机先选「红/蓝 + 1/3/4/6 号席位」，程序按席位换成**步兵、重装、指挥手**三种界面。三种界面共用状态条、顶栏、提醒和弹窗，只在固定的几个格子里换面板。

- **最后效果**：同目录 `role-layouts/` 下四张图已换成实现后的真实截图（2026-10-30）：`picker.png`（选席位页，`picker_shot` 出图）、`infantry.png`（步兵 1440x900）、`hero.png`（重装 1920x1080）、`commander.png`（指挥手 1920x1080），后三张由 `scripts/shoot_roles.sh` 连模拟器出图，全部尺寸在 `artifacts/roles/`。地图还是 2026 底图，等 2027 场地图到了再换。改造前的截图是 `now-info.png`、`now-video.png`。
- **风格不变**：见下面「风格规定」。
- **分两段做**：
  - A 段不需要新协议的数据，现在就能开工：选席位、登录名、三种布局骨架、重装和指挥手面板的空壳、本地地图标记、全屏到官方显示器、截图验收。
  - B 段要先把这些面板用到的 2027 消息接进来（协议对齐的一部分），再填数据、打开发送按钮。
- **不做**：组网、车端叠加（0x0310 / CustomByteBlock）、复盘报告、键鼠控制、性能体系选择。
- **开工前要拍板**：见「待确认」D1–D6。

---

## 风格规定（不改风格）

- 颜色、字体、字号只用 `qml/Theme.qml` 和 `cpp/theme.h` 里已有的值，不新增颜色。
- 花名册类面板用 `Panel.qml`。功能面板（装配、飞镖、哨兵、战场数据分析）用现有的「card + heading」卡片样式，里面的格子和按钮用 `theme::skewPath` 斜切卡片，画法照 `BattleAnalysisPane`。
- 选席位页用 `Panel.qml` 外壳，席位卡用花名册同款 `RobotCard` 头像。
- 顶栏只在现有空位里加东西：防御状态写在基地血条外侧端的下方；控制度条放在中央面板下方那条空带里（`TopBar` 控件本来就是 128 像素高，QML 内容只用到 106）。
- 状态条里「自检」「战术提醒」按钮保持现在的样子。

---

## 依据（2027 通信协议 V1.0.0，2026-10-29）

| 协议原文 | 出处 | 对本计划的影响 |
| --- | --- | --- |
| 选手端只有重装（0x0101 / 0x0165）、步兵（0x0103–0x0105 / 0x0167–0x0169）、空中系统（0x0106 / 0x016A） | 附录二 | 可选席位只有 1/3/4/6（蓝方 +100）。哨兵 7 号没有操作手 |
| RobotPosition 字段 5：「7/107 为哨兵机器人（仅指挥手客户端生效）」；1.3.1：「云台手可通过选手端大地图向机器人发送固定数据」 | 2.2.13、1.3.1 | 指挥手就是空中系统那个席位（D1 待规则手册确认） |
| 规则手册表 3-1：地面操作手为重装 1、步兵 2、哨兵 0；指挥手 1 人，操作空中系统和飞镖系统的选手端 | 规则手册 3 章 | 席位就是 1、3、4、6（蓝方 +100），D1 已确认 |
| 规则手册 5.7.2：自动哨兵每接受一次指挥手干预（含转发小地图标记）花 50 金币；姿态切换冷却 5 秒 | 规则手册 5.7.2 | 哨兵面板和标记工具条要写明花费、显示冷却 |
| 「MyMqttClient 的 clientID 需填入所连接机器人对应的 ID 编号」 | 第 2 章开头 | 登录名 = 席位车号（红 3 填 `3`，蓝 3 填 `103`） |
| 服务器 192.168.12.1:3333，自定义终端 192.168.12.2 | 第 2 章开头 | 赛场配置 |
| robot_health 按「己方 1、3、4、7，对方 1、3、4、7」发送；robot_bullets 只有己方 | 2.2.4 GlobalUnitStatus | 花名册每方 4 台，空中不占行；发弹量只画我方 |
| base_status：0 有 100% 防御增益，1 无增益且护甲未展开，2 无增益且护甲展开 | 2.2.4 | 顶栏加基地防御状态 |
| total_CP_ally / total_CP_enemy；end_reason 2 = 控制度 | 2.2.4、2.2.3 | 控制度进顶栏 |
| 经济只有己方（remaining_economy、total_economy_obtained） | 2.2.5 GlobalLogisticsStatus | 去掉「敌方经济」 |
| 服务器只发本机坐标（指挥手额外收哨兵）；其他车坐标来自雷达 RadarInfoToClient（对方 1/3/4/7 + 己方 1/3/4/7，单位 cm） | 2.2.13、2.2.19 | 地图按来源画、标时效，没收到的写出来 |
| 重装：AssemblyCommand、TechCoreMotionStateSync、Event 14/15/16 | 2.2.21、2.2.22、2.2.7 | 重装专属「装配」面板 |
| 指挥手：DartCommand / DartSelectTargetStatusSync、SentryCtrlCommand / SentryCtrlResult / SentryStatusSync / RobotPathPlanInfo、MapClickCmd / MapClickInfo | 2.2.17–2.2.18、2.2.28–2.2.32 | 指挥手专属「飞镖」「哨兵」「标记」 |
| 云台手地图标记两次间隔不得低于 0.5 秒；DartCommand、SentryCtrlCommand、AssemblyCommand 最高 10Hz | 1.3.1、2.1 | 发送限频 |
| 坐标原点：红方补给站附近围挡交点；X 沿长边指向蓝方，Y 沿短边指向红方停机坪；0x0303 地图坐标单位 m | 1.2 己方机器人位置表下方、表 1-35 | 地图点击换算 |

---

## 最后呈现效果

### 1. 开机选席位（`picker.png`）

- 程序启动先出这一页。这时还不连网络，也不显示主界面。
- 分两步：阵营（红/蓝），席位（1 重装 / 3 步兵 / 4 步兵 / 6 指挥手）。两步都不预选，都点过之后「确认并进入」才能点。
- 确认后：登录名 = 车号（蓝方 +100），布局按席位定。本次运行不能改，换席位要重启。
- 状态条最左边一直显示「红方 3 号 · 步兵」。
- 跳过这一页：设了 `RM_SELF_ID`（模拟器启动脚本、验收脚本、CI），或者命令行给了 `--seat 103`。
- 连上服务器后核对：服务器发来的 RobotStaticStatus.robot_id 和所选不一致时，自检页标红「席位选错：选的是红方 3 号，服务器说是蓝方 103 号」，同时写日志。这项要 B 段才有数据，A 段先显示「未核对」。

### 2. 步兵布局（`infantry.png`，屏幕尺寸待定，先按 1440×900）

骨架和现在一样，变的是格子里的内容。

| 位置 | 内容 | 数据来源 |
| --- | --- | --- |
| 状态条 | 席位标签、四个链路灯、REC、自检、战术提醒 | 本地 |
| 顶栏 | 校名、基地血量 + 防御状态、前哨血量、比分、局数、倒计时、控制度对比条 | GameStatus、GlobalUnitStatus |
| 左 | 我方 4 台：血量、剩余发弹量、战亡 | GlobalUnitStatus |
| 中 | 2027 场地图：自机、雷达位置（标「几秒前」）、指挥手发来的标记、四个阵地的控制状态和推进进度 | RobotPosition、RadarInfoToClient、MapClickInfo、GlobalUnitStatus、GlobalSpecialMechanism |
| 右 | 敌方 4 台：血量，残血闪烁 | GlobalUnitStatus |
| 下左 | 战场事件 | Event |
| 下中 | 战场数据分析：顶上一条堡垒状态；四格 我方经济 / 总伤害 / 击杀 / 能量机关；下面四个阵地小卡片 | GlobalSpecialMechanism、GlobalLogisticsStatus、GlobalUnitStatus、Event 1（击杀在本机累计）、RuneStatusSync |
| 下右 | 图传缩略图，点击放大 | UDP 3334 |

### 3. 重装布局（`hero.png`，1920×1080 官方显示器）

- 地图横跨中间两列。下排四格：战场事件 / 战场数据分析 / **装配** / 图传。
- 装配面板显示：可选最高难度、科技等级、42mm 发射机构状态；放入、平移、旋转三步的进度；本步剩余和总剩余时间；对方是否在装配四级；最近一次装配结果（Event 15 的 0–8 号原因翻成中文）；机械臂帧率和关节角。
- 按钮：开始兑换 / 确认装配 / 取消装配。A 段是灰的，B 段才能点。

### 4. 指挥手布局（`commander.png`，1920×1080 官方显示器）

- 左列上下两块：我方 4 台（哨兵卡片多一行姿态），敌方 4 台。
- 中间：大地图占两行，下面是标记工具条。
  - 点空地标位置，点敌方车标目标。
  - 类型：攻击 / 防御 / 警戒 / 自定义字母（只允许 C–L、N、O、Q–Z）。
  - 发给：全体含哨兵 / 除哨兵 / 指定车。
  - 地图上画哨兵规划路径（RobotPathPlanInfo），以及已发出和收到的标记。
- 右列：
  - 飞镖：闸门状态、已选目标、五个目标按钮、开闸、确认发射（按住 1 秒）。
  - 哨兵：姿态、是否弱化/强化、控制方式、血量和发弹量、6 个姿态按钮、补弹/回血/复活、上一条指令的结果。
- 下排：战场事件、战场数据分析（压缩成堡垒状态加四个阵地）、图传缩略图。
- 所有发送按钮：断线时整块变灰不能点；按协议限频；发出后等回显（哨兵看 SentryCtrlResult，飞镖看 DartSelectTargetStatusSync 有没有变），2 秒没变就提示「未确认」；断线期间的点击直接丢弃，不排队补发。

### 5. 三种布局共同的变化

- 状态条去掉「模式 Info · 原因 Startup」，换成席位标签。模式和原因照样写日志、照样进 `--dump-layout`。
- 顶栏加基地防御状态（血条外侧端下方）和控制度对比条（中央面板下方）。
- 花名册每方 4 台（1、3、4、7）。

---

## 待确认（开工前拍板）

- **D1 指挥手坐哪个席位**：已确认。2027 规则手册表 3-1：指挥手操作「空中系统、飞镖系统」对应的选手端，并可配置自定义终端；2027 取消了云台手和飞手。所以指挥手就是 6/106 号空中系统席位。
- **D2 步兵席位的屏幕**：方案文档的路由图写明官方 1080p 显示器只在重装、指挥手席位。步兵席位的终端用什么屏、多大分辨率？没定之前按 1280×760（程序最小尺寸）和 1440×900 两档验收。
- **D3 机械臂接在哪个席位**：示意图按「接在重装席位」画（装配要用），其他席位的控制器灯显示「未启用」。
- **D4 发送按钮防误触**：建议花钱或撤不回的操作（确认发射、花金币复活、取消装配）用按住 1 秒，其余单击。也可以统一改成弹窗二次确认。
- **D5 每次启动都必须手动选，没有默认值**（沿用现在启动脚本的规定）。如果嫌每局都要点两步，可以改成记住席位号、每次只问红蓝。
- **D6 顶栏校名**：2027 协议不发队名。可以在选席位页顺便填对手校名，也可以写进配置文件。

---

## 施工步骤

约定：每一步都是代码和测试一起交；C++17 / Qt6，遵守 `QT_NO_KEYWORDS`（用 `Q_SIGNALS`、`Q_EMIT`）；新测试注册进 `CMakeLists.txt` 的测试段；GUI 测试设 `QT_QPA_PLATFORM=offscreen`；时间一律用 `monotonic_now()`；QML 文件注册进 `resources.qrc`。

### A 段：不依赖新协议，现在就能做

**A1 席位模型**（小）
- 新增 `cpp/seat.h`、`cpp/seat.cpp`、`cpp/seat_test.cpp`。
- `Seat` 只存 robot_id，提供：阵营（1–99 红，101–199 蓝）、角色（1→重装，3/4→步兵，6→指挥手）、标签文字（「红方 3 号 · 步兵」）、MQTT 登录名（`"3"` / `"103"`）。
- 合法席位只有 1、3、4、6、101、103、104、106。`cpp/config.cpp` 的 `valid_self_robot_id` 改用它；`cpp/main.cpp` 里 RM_SELF_ID 的报错文字同步改。模拟器照样可以发 7 号的数据，只是不能选 7 号当自机。
- 验收：`seat_test` 覆盖全部合法和非法编号、红蓝换算、标签、登录名；`config_test` 里 7/107 改为拒绝。

**A2 登录名改成车号**（小）
- `cpp/mqtt_intake.cpp`：`mosquitto_new` 的 client id 从 `"rm-terminal"` 改成 `Seat` 给的登录名。`MqttIntake` 构造时已经拿到 `self_robot_id`，不用改调用方。
- 验收：单测检查登录名；再手动跑一次 `mosquitto -p 3333 -v`，日志里应出现 `New client connected ... as 3`，把这行贴进证据。

**A3 开机选席位页**（中）
- 新增 `qml/SeatPicker.qml`（用 `Theme.qml` 的颜色和字号）和 `cpp/seat_picker.h`、`cpp/seat_picker.cpp`（用 QQuickWidget 包一层，返回选中的 `Seat`，或者表示用户取消）。
- `cpp/main.cpp`：席位来源的优先级是 `--seat` > `RM_SELF_ID` > 选席位页。选席位页在创建 `MqttIntake`、`Dashboard` 之前弹出。用户取消就正常退出（退出码 0，日志 `seat_cancelled`）。`--safe-smoke`、`--diagnostic`、`--replay` 不弹；回放用录像里记的 self_robot_id。
- 新增命令行参数 `--seat <id>`：同步更新「Unknown argument」的提示文字和 `cpp/assert_bad_args.cmake`；`--seat 7` 返回退出码 2。
- `启动.command`：保留终端里的选择（模拟器也要知道自机），列表改成 8 个合法席位，选完导出 `RM_SELF_ID`，这样终端程序不会再弹一次。
- 日志：`startup` 记录里加 `seat=3 role=infantry source=picker|env|cli`。
- 验收：`seat_picker_test`（offscreen）：没选完时确认按钮不可用；选红方 + 3 号返回 3；选蓝方 + 6 号返回 106。仿照 `popup_shots` 加一个出图程序，生成真实的 `picker.png`。

**A4 布局档案**（大）
- 新增 `cpp/layout_profile.h`、`cpp/layout_profile.cpp`、`cpp/layout_profile_test.cpp`：三份档案（步兵、重装、指挥手），每份写明网格的列宽、行比例、每个格子放哪个面板。
- `cpp/dashboard.h`、`cpp/dashboard.cpp`：构造函数改成 `Dashboard(const Config&, Role)`，按档案把面板放进 `QGridLayout`。现有面板（花名册、地图、事件、战场数据分析、图传）内部不改，只换位置；A6 的新面板在这里接入。
- `layoutDump()` 加 `role` 和每个格子的面板名，仍然固定顺序、只含整数，保证同样输入得到逐字节相同的 JSON。
- `cpp/dashboard_layout_test.cpp`：三种档案 × {1280×760, 1920×1080} 都检查面板不重叠、不出窗口、地图保持 28:15、两侧花名册等宽、档案里的面板都可见。步兵档案在 1280×720 仍然要过现有的 720p 断言。
- 验收：`ctest -R 'layout_profile|dashboard_layout'`。

**A5 状态条和 REC 标签**（小）
- `cpp/dashboard.cpp`：`mode_banner_` 改为显示席位标签，不再显示模式和原因（这两项照写日志、照进 `layoutDump`）。按钮样式不动。
- 「● REC」标签宽度固定（`QSizePolicy::Fixed`，和链路灯一样）。现在有录制、又没有告警时，它会被拉成一长条红色。
- 验收：布局测试断言席位标签文字、REC 标签宽度不随窗口变；截图人工看一遍。
- 事件时间被截成「13:20:5」的问题已在 2026-10-29 单独改掉（时间改用小一号的字），不在本步。

**A6 重装、指挥手面板（空壳）**（中）
- 新增 `cpp/assembly_pane.*`（重装）、`cpp/dart_pane.*`、`cpp/sentry_pane.*`（指挥手），配一个 `cpp/role_panes_test.cpp`。
- 每个面板的输入是一个纯数据结构（`AssemblyView`、`DartView`、`SentryView`）。A 段测试直接喂假值，B 段改由 `Snapshot` 填。
- A 段只画结构：数值显示「--」并写明在等哪条消息（例如「等待 TechCoreMotionStateSync」）；按钮全部禁用，悬停提示「协议接入后开放」。
- 验收：每个面板喂假值后 grab 成图，断言关键文字存在、按钮是禁用状态。

**A7 本地地图标记（指挥手）**（中）
- 新增 `cpp/map_marks.h`、`cpp/map_marks.cpp`、`cpp/map_marks_test.cpp`。标记的字段直接照 MapClickCmd 定义，B 段发送时不用再改结构。注意：proto 里 `mode` 是**标记类型**（攻击/防御/警戒/自定义），`type` 是**标记模式**（地图/对方机器人），名字容易弄反。
- 像素和场地坐标（米）的换算复用 `map_viewport` 和 `FieldExtent`；原点和方向按协议（红方补给站附近围挡交点，X 指向蓝方，Y 指向红方停机坪）。我方是蓝方、画面翻转时也要对。
- `cpp/map_pane.h`、`cpp/map_pane.cpp`：只有指挥手档案下地图可以点。点空地 = 位置标记，点敌方车 = 目标标记；工具条选类型和发送范围；标记画在地图上，30 秒后淡出（时长写进配置）。A 段只存在本机，不发送。
- 验收：`map_marks_test`：四个角和中心点往返换算误差小于 1 cm，红蓝视角各测一遍；不允许的字母被拒绝；两次标记间隔不到 0.5 秒时第二次被拦下。

**A8 全屏到官方显示器**（小）
- 新增配置 `display_fullscreen`（默认 false）和 `display_screen`（屏幕名，空 = 主屏）；`cpp/main.cpp` 按配置在指定屏幕上全屏。
- 验收：`config_test` 覆盖新键（写错要大声失败并指出行号，和现有配置一致）；实机接 1080p 显示器跑一次，截图存证。

**A9 截图与布局证据**（小）
- `cpp/main.cpp`：offscreen 截图现在固定 1440×900，加环境变量 `RM_TERMINAL_CAPTURE_SIZE=1920x1080` 可改尺寸。
- 新增 `scripts/shoot_roles.sh`：三种席位 × 两种分辨率，各出一张截图和一份布局 JSON，存到 `artifacts/roles/`。
- 验收：跑完得到 6 张图和 6 份 JSON，JSON 里的 role 和面板齐全；把截图和示意图放在一起对比。

### B 段：先接对应的 2027 消息，再填数据、开按钮

前置条件：协议对齐把下表的消息接进 `proto/rm_terminal.proto` → `cpp/decoder.cpp` → `cpp/conversion.cpp` → `cpp/domain.h` → `cpp/store.cpp`，模拟器同步发送。协议对齐整体另写计划，这里只列本计划用到的。

| 消息 | 用在哪 |
| --- | --- |
| GlobalUnitStatus | 顶栏防御状态和控制度、花名册 4+4、战场数据分析 |
| GlobalSpecialMechanism | 阵地推进进度、堡垒计时 |
| GlobalLogisticsStatus | 我方经济、科技等级 |
| RobotStaticStatus | 席位核对 |
| RobotPosition（含 robot_id） | 自机；指挥手看哨兵 |
| RadarInfoToClient | 地图上队友和敌方的位置 |
| MapClickInfo / MapClickCmd | 标记收发 |
| TechCoreMotionStateSync / AssemblyCommand / Event 14–16 | 装配 |
| DartSelectTargetStatusSync / DartCommand / Event 9–10 | 飞镖 |
| SentryStatusSync / RobotPathPlanInfo / SentryCtrlCommand / SentryCtrlResult | 哨兵 |
| RuneStatusSync | 战场数据分析里的能量机关 |

**B1 共享面板换数据**：花名册改成 4+4（robot_health 顺序是己方 1、3、4、7，对方 1、3、4、7；robot_bullets 协议没写顺序，先按 1、3、4、7 处理，联调时核对）；顶栏接防御状态和控制度条；战场数据分析换数据源，击杀数由 Event 1 在本机累计（终端中途重启会从 0 重新数）；去掉敌方经济和加固两项（终端收不到）。后勤系数保留，按规则手册 5.6.1 在本机算：λ = 1 − 己方基地已损失血量 / 20000，双方都能算；它决定复活读条、回血速度和图传清晰度（分辨率按 λ² 下降，自定义终端的图传也一样）。验收：改写 `battle_analysis_test`、`roster_runtime`、`top_bar_model` 的断言，出截图。

**B2 席位核对**：RobotStaticStatus.robot_id 和所选不一致时，自检页出红行，写日志 `seat_mismatch`；一致时显示绿色「席位正确」。验收：自检相关测试加这两种情况。

**B3 装配**：`AssemblyView` 改由 Snapshot 填；Event 15 的原因码 0–8 翻成中文；按钮发 AssemblyCommand（operation 0/1/2 加 difficulty），断线时禁用，最高 10 次/秒，「取消装配」按住 1 秒。验收：编码单测逐字段比对；模拟器演一遍完整装配流程。

**B4 指挥手**：
- 飞镖：`DartView` 由 DartSelectTargetStatusSync 填；选目标、开闸、确认发射都发 DartCommand；确认发射按住 1 秒；发出后 2 秒内状态没变就提示「未确认」。
- 哨兵：`SentryView` 由 SentryStatusSync 填；规划路径画到地图上（RobotPathPlanInfo：起点单位分米，加 49 个增量）；按钮发 SentryCtrlCommand（command_id 1、3–12），按 SentryCtrlResult 显示成功或失败。自动哨兵模式下每个按钮旁写明「50 金币」；姿态按钮在 5 秒冷却内变灰。
- 标记：A7 的本地标记改为发 MapClickCmd；收到的 MapClickInfo 在所有席位的地图上都画出来，步兵也能看到指挥手的标记。
- 雷达位置：RadarInfoToClient 画到地图上，cm 换算成 m。串口 0x0305 的备注是「x、y 均为 0 视为未发送」，RadarInfoToClient 是否同样处理，联调时确认。
- 验收：每类指令都有编码单测，逐字段比对；模拟器收到指令后回状态（见 B5）。

**B5 模拟器与验收脚本**：`sim/match_server.py` 发上面这些消息，接收 DartCommand、SentryCtrlCommand、MapClickCmd、AssemblyCommand 并按规则回状态；`sim/control_panel.py` 加对应按钮；新增 `scripts/accept_roles.py`，三种席位各跑一遍，通过日志和布局 JSON 断言界面状态。

### C 段：实机验收

- 重装、指挥手：终端接官方同款 1080p 显示器全屏跑，模拟器演一局。截图，并由人确认：字不糊、按钮够大好点、坐在座位上看得清。
- 步兵：D2 定下来以后，在实际的终端屏上跑一遍。
- 三种席位各拔一次网线：发送按钮变灰；恢复后能用；断线期间的点击不补发。

---

## 测试命令

```sh
cmake --build build/ubuntu --parallel
ctest --test-dir build/ubuntu --output-on-failure
```

每一步做完都跑全量测试。B5 之后再跑 `scripts/accept_roles.py`，以及现有的 `scripts/accept_links.py`、`scripts/accept_alerts.py`。

## 不做

- 组网、0x0310 叠加（CustomByteBlock）、复盘报告、KeyboardMouseControl、RobotPerformanceSelectionCommand、RuneActivateCommand。
- 比赛中切换席位。
- 断线期间的指令排队补发（沿用控制器转发「绝不补发」的规则）。
- 删除或放宽现有测试；改动与布局无关的面板内部实现。

## 风险

- D1 如果不成立，只改席位表；指挥手布局本身不受影响。
- 1080p 下现有 QML 顶栏和花名册的字号可能偏小，A9 出图后可能要调一轮。
- 地图坐标的原点和方向协议只有文字描述，要用 `RMUC2027_V1.0.0.stp` 导出的场地图核对一次（放在换 2027 场地图的任务里做）。
- 飞镖、花金币复活这类指令误发的代价大，D4 必须在 B3、B4 之前定下来。
