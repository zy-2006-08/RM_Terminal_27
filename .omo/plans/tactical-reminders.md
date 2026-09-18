# tactical-reminders - Work Plan

## TL;DR (For humans)
<!-- Fill this LAST, after the detailed plan below is written, so it summarizes the REAL plan. -->
<!-- Plain English for a non-engineer: NO file paths, NO todo numbers, NO wave/agent/tool names. -->

**What you'll get:** 终端右上角的战术提醒设置入口；赛前填写并永久保存多条倒计时提醒，赛中自动离线播报两遍。

**Why this approach:** 跟随比赛真实倒计时，赛前准备语音；语音后端独立，先支持 macOS，便于以后适配其他系统。

**What it will NOT do:** 不发送机器人指令，不增加网络服务，不实现 Windows/Linux 语音，不补播断线期间过时战术。

**Effort:** Large
**Risk:** Medium - 当前比赛数据没有唯一场次标识，断线换局只能保守处理。
**Decisions to sanity-check:** 相邻提醒串行排队；无法确认场次连续性时暂停本局提醒，不猜测重置。

Your next move: 在执行会话中运行 `/start-work tactical-reminders`。本文件的创建不代表功能已实现。

---

> TL;DR (machine): Large / Medium; Qt desktop editor, atomic persistence, countdown scheduler, prepared offline macOS audio, deterministic tests.

## Scope
### Must have
- 项目根目录 `/Users/zy/RMGF/terminal`；用户已确认全部产品范围和本地 macOS 优先方案。
- 顶部右侧固定按钮显示启用状态与启用条数；只手动打开设置。赛前添加、编辑、删除、试听、保存，字段为稳定 ID、剩余 MM:SS、文字、单项启用。总开关默认关闭。
- 比赛中及曾处于比赛但当前失联时，设置只读，禁止试听和重新启用；允许关闭总开关，立即停止播放、清空队列并保存关闭状态。比赛开始时取消试听并锁定已打开窗口，未保存草稿不应用。
- 配置与音频就绪状态分离：原子保存 JSON 后离线准备音频；准备失败可见，不声称音频可用。恢复启动检查缓存，不自动播报准备期间错过的内容。
- macOS 使用本机中文语音离线生成音频文件，触发时只播放文件；使用异步进程、参数数组，无 shell 插值。查明系统工具真实参数后实现，缺中文声音时显示可操作错误。
- 每条两次总播放；第一遍成功完成后至少 1000ms 再播第二遍。单队列 FIFO，同一条两遍不交错；同刻按保存顺序，跳过多个阈值按剩余秒数降序。设置页对同刻/过近提醒提示排队延迟。
- stage=4 才触发；使用各字段独立质量和新鲜度，不使用模拟器的 elapsed 或固定 round。持续健康样本满足 previous > threshold >= current 时触发，重复快照不重复处理；派发即标记已消费，失败不自动重试。
- 沿用配置 freshness_stale_ms（默认2200ms），同时检查观察间隔，避免事件循环卡顿漏判失联。需要新鲜合法阶段、倒计时、明确未暂停及连接状态才启动播放。缺少暂停信息则显示等待状态而非猜测。
- 新鲜暂停允许当前一句结束，阻止第二遍和下一条，恢复后继续队列；明确断线/必需字段失效取消待播与重复定时器，暂停本局自动提醒。结束比赛或关闭开关立即终止当前音频。
- 已连续观察到新鲜非4阶段再进入4，且取得属于新阶段的新倒计时样本，才确认下一局；包括3→4及模拟器5→4。不能使用阶段变化前缓存的倒计时。新基线及恰好等于基线的阈值不补播。
- 首次启动已经在比赛中时建立基线，只允许之后的阈值；进程内断线重连不能走这个首次启动特例。不能确认是否换局的断线恢复保持暂停，直到确认下次开赛。倒计时上升不重置已消费项，显示连续性不确定。
- 生成/播放/停止使用代次标识，旧异步回调不得激活新配置或已取消任务。退出程序清理子进程和定时器。
### Must NOT have (guardrails, anti-slop, scope boundaries)
- 不添加 HTTP/WebSocket 服务，不复用控制话题，不执行提醒文本为命令，不依赖云语音。
- 不修改无关 Store 并发、模拟器计时或既有配置问题。保持 Linux 可编译但明确无语音后端；不宣称当前支持 Windows 构建。
- 不持久化待播队列或伪造场次标识；不静默覆盖损坏的配置；不自动提交代码。

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: TDD + existing C++ CTest executables, injected telemetry/time/audio/process results. No sleeping in unit tests.
- 构建：`cmake -S . -B build/reminders -DBUILD_TESTING=ON -DRM_TERMINAL_INTEGRATION_TESTS=ON`；`cmake --build build/reminders --parallel`；`ctest --test-dir build/reminders --output-on-failure`。执行前核对真实 CMake 选项和现有依赖，缺依赖如实记录。
- 音频自动验证进程退出、生成文件、时序及停止；实际声卡/耳机是否可听不能由退出码证明，无回环采集条件时明确报告此验证限制，不编造听感验收。
- Evidence: <attemptDir>/task-<N>-tactical-reminders.<ext> (attemptDir = currentAttemptDir from 'omo ulw-loop status --json', .omo/evidence/ulw/<session>/<goalId>/a<attempt>; outside ulw-loop use .omo/evidence/)

## Execution strategy
### Parallel execution waves
Wave 1: 1（冻结共享模型）；Wave 2: 2、3、4并行（不同文件，构建注册串行合入）；Wave 3: 5；然后最终验证。

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1 | none | 2,3,4 | none |
| 2 | 1 | 5 | 3,4 |
| 3 | 1 | 5 | 2,4 |
| 4 | 1 | 5 | 2,3 |
| 5 | 2,3,4 | F1-F4 | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [ ] 1. 定义共享提醒模型与确定性调度器并测试
  Resumed: 用户撤销不使用子代理限制，由 Sisyphus 负责实施与验证。
  What to do / Must NOT do: 新增 cpp/tactical_reminder.h、reminder_scheduler.{h,cpp}、reminder_scheduler_test.cpp；显式时间和连接/字段状态输入，输出排队/停止/状态事件。冻结 ID、配置、语音准备及播放接口契约，不耦合 UI 或真实音频。
  Parallelization: Wave 1 | Blocked by: none | Blocks: 2,3,4
  References: cpp/domain.h:18-48、cpp/store.cpp:194-208、cpp/ui_mode_test.cpp、cpp/popup_state_test.cpp、proto/rm_terminal.proto。
  Acceptance criteria: CTest reminder_scheduler；覆盖61→59触发60一次、重复59、3→4缓存计时不触发、5→4重新布防、初次59跳过60但31→29触发30、倒计时回升不重播。
  QA: `ctest --test-dir build/reminders -R '^reminder_scheduler$' --output-on-failure`；失败路径新鲜阶段但倒计时超时、70断线50恢复不补60、暂停字段缺失不启动；证据 .omo/evidence/task-1-tactical-reminders.txt。
  Commit: N
- [ ] 2. 原子配置存储与失败恢复测试
  What to do / Must NOT do: 新增 cpp/reminder_repository.{h,cpp} 与测试；QStandardPaths 稳定应用目录 tactical_reminders.json，版本1，字段 master_enabled/reminders，每项 id/remaining_sec/text/enabled；使用 QSaveFile 禁用非原子回退。限制100条、文字1–200 Unicode字符、秒数0–5999，ID唯一且不得由文本充当路径。损坏文件保留并报错，丢失文件加载空且关闭。临时目录可注入。
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 5
  References: cpp/config.{h,cpp}（仅参考错误表达，不扩展扁平配置）；CMakeLists.txt 既有测试注册。
  Acceptance criteria: 中文往返和关闭状态持久；格式、范围、重复ID和未知版本拒绝；保存失败旧文件完整。
  QA: `ctest --test-dir build/reminders -R '^reminder_repository$' --output-on-failure`；测试正常重载与注入替换失败；证据 .omo/evidence/task-2-tactical-reminders.txt。
  Commit: N
- [ ] 3. 离线语音准备、缓存和两遍串行播放
  What to do / Must NOT do: 新增 cpp/speech_backend.h、macos_speech_backend.{h,cpp}、reminder_playback.{h,cpp} 及测试；缓存键由文字、声音及版本哈希产生，原子落盘；系统 say 生成、afplay 播放或经核实的等效本机实现，均异步且不经过shell；其他平台 unavailable 实现。不允许触发时临时合成。准备任务过期、禁用、结束均取消或忽略旧回调；后台失败明确传播。
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 5
  References: cpp/video_receiver.cpp 的QProcess生命周期、CMakeLists.txt 平台条件；实施时查阅本机 say/afplay 手册核对参数。
  Acceptance criteria: fake音频完成t=4000，t=4999无第二遍，t=5000可开始；正常两遍恰好两次；队列不重叠；暂停门控、旧回调、缺声音、超时、非零退出、损坏缓存全部有测试。
  QA: `ctest --test-dir build/reminders -R '^reminder_(audio|playback)$' --output-on-failure`；真实macOS生成中文非空可解码音频并验证播放进程/停止，无外网请求；证据 .omo/evidence/task-3-tactical-reminders.txt。
  Commit: N
- [ ] 4. 设置窗口与顶部入口及交互测试
  What to do / Must NOT do: 新增 cpp/tactical_reminder_dialog.{h,cpp}，修改 cpp/top_bar.{h,cpp}、qml/TopBar.qml，通过共享模型/信号输出动作；阶段三前使用测试模型，不提前接入未完成控制器。固定右侧按钮、数量/错误/准备状态、单项编辑、试听、草稿取消、保存反馈。比赛及不确定状态只读且仅可关闭总开关，不自动弹出。
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 5
  References: cpp/top_bar.cpp:45-80、qml/TopBar.qml、qml/Theme.qml、cpp/dashboard_layout_test.cpp；遵守 QT_NO_KEYWORDS。
  Acceptance criteria: 1280×760及1440×900无遮挡；中文无裁剪；模拟比赛开始立即锁编辑且取消试听请求；保存失败保留草稿/错误，不误报成功。
  QA: `ctest --test-dir build/reminders -R '^reminder_dialog$' --output-on-failure`；原生截图覆盖赛前/赛中/失联/错误，使用 frontend 与 visual-qa 技能；证据 .omo/evidence/task-4-tactical-reminders/。
  Commit: N
- [ ] 5. 应用装配、连接状态与完整回归
  What to do / Must NOT do: 新增 cpp/tactical_reminder_controller.{h,cpp} 与测试；cpp/main.cpp共享同一快照和monotonic时间更新，cpp/dashboard.{h,cpp}连接入口/窗口，cpp/mqtt_intake.{h,cpp}提供typed连接/断线信号并排队到GUI线程；CMakeLists.txt串行整合全部目标。设置明确稳定应用存储名，避免改变既有无关行为。关闭立即生效，持久化失败仍保持本次进程关闭并提示。
  Parallelization: Wave 3 | Blocked by: 2,3,4 | Blocks: F1-F4
  References: cpp/main.cpp:214-269、cpp/dashboard.cpp:415-657、cpp/mqtt_intake.cpp、cpp/dashboard_layout_test.cpp、scripts/verify_mode_regression.sh。
  Acceptance criteria: 注入比赛序列验证端到端触发、下一局、失联锁定；重新构建控制器/应用加载持久配置；完整CTest与现有布局回归通过。文档说明macOS中文声音前提、离线准备、失联保守限制及其他平台未提供语音。
  QA: `ctest --test-dir build/reminders --output-on-failure`；真实原生应用入口、持久化、中文音频准备和停止测试；无设备声学证据须如实标记未验证；证据 .omo/evidence/task-5-tactical-reminders.txt。
  Commit: N

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [ ] F1. Plan compliance audit — Oracle只读逐条对照已批准范围与测试证据，重点两遍间隔及非4→4；结果 .omo/evidence/f1-compliance.md。
- [ ] F2. Code quality review — 检查异步取消、配置原子性、GUI线程边界、shell注入及缓存路径；结果 .omo/evidence/f2-quality.md。
- [ ] F3. Real manual QA — Agent执行完整CTest与原生窗口/音频进程检查，截图与录音能力限制明确报告；结果 .omo/evidence/f3-qa.md。
- [ ] F4. Scope fidelity — 核对仅预期文件改动，不混入已有工作区改动或机器人控制/网络服务；结果 .omo/evidence/f4-scope.md。

## Commit strategy
用户未授权提交，禁止提交或推送。执行前记录已有工作区改动并保护；加载git-master后才能运行git操作。

## Success criteria
全部1–5及F1–F4有实际证据，未通过项明确报告；用户可在macOS配置、保存、准备离线语音并在比赛中获得符合时序的提醒；没有把进程成功等同于耳机可听，没有宣称已适配Windows/Linux语音。
