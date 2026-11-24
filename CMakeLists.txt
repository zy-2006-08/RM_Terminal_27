cmake_minimum_required(VERSION 3.21)
project(rm_terminal VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Qml Quick QuickWidgets)
find_package(Protobuf REQUIRED)
find_package(absl CONFIG REQUIRED)
find_library(MOSQUITTO_LIBRARY NAMES mosquitto REQUIRED)
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h REQUIRED)
set(PROTO_CPP ${CMAKE_CURRENT_BINARY_DIR}/rm_terminal.pb.cc)
set(PROTO_HPP ${CMAKE_CURRENT_BINARY_DIR}/rm_terminal.pb.h)
add_custom_command(OUTPUT ${PROTO_CPP} ${PROTO_HPP}
    COMMAND protobuf::protoc --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/proto ${CMAKE_CURRENT_SOURCE_DIR}/proto/rm_terminal.proto
    DEPENDS proto/rm_terminal.proto)
add_library(terminal_proto ${PROTO_CPP})
target_include_directories(terminal_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(terminal_proto PUBLIC protobuf::libprotobuf absl::log_internal_check_op absl::log_internal_message)

# 源码分在 cpp/ 下面的 12 个子文件夹里（各文件夹管什么见 README 的目录说明）。
# #include 只写文件名，所以每个放头文件的子文件夹都要在查找范围里。
set(RM_TERMINAL_CPP_DIRS
    cpp/app cpp/data cpp/video cpp/recording cpp/voice
    cpp/layout cpp/panes cpp/map cpp/ui cpp/game)

add_library(terminal_logging cpp/app/logging.cpp cpp/app/logging.h cpp/app/config.cpp cpp/app/config.h cpp/app/seat.cpp cpp/app/seat.h)
target_include_directories(terminal_logging PUBLIC ${RM_TERMINAL_CPP_DIRS})
target_link_libraries(terminal_logging PUBLIC Qt6::Core)

add_library(terminal_domain cpp/data/conversion.cpp cpp/data/store.cpp cpp/data/decoder.cpp cpp/game/match_event.cpp cpp/game/match_event.h cpp/app/clock.cpp cpp/app/clock.h
    cpp/game/radar_intel.cpp cpp/game/radar_intel.h)
target_include_directories(terminal_domain PUBLIC ${RM_TERMINAL_CPP_DIRS})
target_link_libraries(terminal_domain PUBLIC terminal_proto)
add_library(terminal_video cpp/video/video.cpp cpp/video/video_receiver.cpp cpp/video/video_receiver.h cpp/video/video_recovery.cpp cpp/video/video_recovery.h
    cpp/video/video_relay.cpp cpp/video/video_relay.h)
target_include_directories(terminal_video PUBLIC ${RM_TERMINAL_CPP_DIRS})
target_link_libraries(terminal_video PUBLIC terminal_logging Qt6::Core Qt6::Network)
# 录像格式与 GUI/网络无关,单独成库:格式测试不必拉起 Qt Widgets 或一个 broker。
add_library(terminal_recording cpp/recording/recorder.cpp cpp/recording/recorder.h)
target_include_directories(terminal_recording PUBLIC ${RM_TERMINAL_CPP_DIRS})
target_link_libraries(terminal_recording PUBLIC Qt6::Core)
# 自定义控制器串口链路同样与 GUI/网络无关:测试用一对 pty 就能覆盖插拔和坏帧。
add_library(terminal_controller
    cpp/data/referee_frame.cpp cpp/data/referee_frame.h
    cpp/data/controller_link.cpp cpp/data/controller_link.h)
target_include_directories(terminal_controller PUBLIC ${RM_TERMINAL_CPP_DIRS})
target_link_libraries(terminal_controller PUBLIC terminal_domain terminal_logging Qt6::Core)
add_library(terminal_presentation cpp/ui/presentation.cpp cpp/layout/ui_mode.cpp cpp/layout/ui_mode.h cpp/ui/popup_state.cpp cpp/ui/popup_state.h cpp/game/game_result.cpp cpp/game/game_result.h cpp/map/field_map.cpp cpp/map/field_map.h cpp/panes/top_bar_model.cpp cpp/panes/top_bar_model.h cpp/voice/reminder_scheduler.cpp cpp/voice/reminder_scheduler.h cpp/voice/tactical_reminder.h
    cpp/data/link_health.cpp cpp/data/link_health.h
    cpp/game/match_alerts.cpp cpp/game/match_alerts.h
    cpp/layout/layout_profile.cpp cpp/layout/layout_profile.h
    cpp/map/map_marks.cpp cpp/map/map_marks.h cpp/game/next_step.cpp cpp/game/next_step.h
    cpp/game/buff_ledger.cpp cpp/game/buff_ledger.h
    cpp/map/field_zones.cpp cpp/map/field_zones.h
    cpp/game/overview.cpp cpp/game/overview.h cpp/recording/match_report.cpp cpp/recording/match_report.h
    cpp/map/map_effects.cpp cpp/map/map_effects.h)
target_link_libraries(terminal_presentation PUBLIC terminal_domain terminal_logging Qt6::Core)

# 操作手个性化界面:方案模型、藏面板后的补位规则、存盘。只链 Qt6::Core,
# 和战术提醒的持久化一样,测试不必拉起界面。
add_library(terminal_ui_profiles
    cpp/layout/ui_profile.cpp cpp/layout/ui_profile.h
    cpp/layout/layout_ops.cpp cpp/layout/layout_ops.h
    cpp/layout/ui_profile_store.cpp cpp/layout/ui_profile_store.h
    cpp/app/terminal_settings.cpp cpp/app/terminal_settings.h)
target_include_directories(terminal_ui_profiles PUBLIC ${RM_TERMINAL_CPP_DIRS})
target_link_libraries(terminal_ui_profiles PUBLIC terminal_presentation Qt6::Core)
target_compile_definitions(terminal_ui_profiles PRIVATE QT_NO_KEYWORDS)

# 战术提醒的持久化和语音后端。独立于 terminal_presentation:调度器是纯逻辑,
# 而这一层要碰文件系统和子进程,分开后纯逻辑测试不必链接这些副作用。
add_library(terminal_reminders
    cpp/voice/reminder_repository.cpp cpp/voice/reminder_repository.h
    cpp/voice/reminder_playback.cpp cpp/voice/reminder_playback.h
    cpp/voice/tactical_reminder_controller.cpp cpp/voice/tactical_reminder_controller.h
    cpp/voice/alert_voice.cpp cpp/voice/alert_voice.h
    cpp/voice/speech_backend.h)
target_include_directories(terminal_reminders PUBLIC ${RM_TERMINAL_CPP_DIRS})
target_link_libraries(terminal_reminders PUBLIC terminal_presentation Qt6::Core)
# 语音后端按平台条件编译。macOS 用系统 say;Linux 用 scripts/setup_voice.sh 装的
# Piper「超文」(操作手试听后选定),没装时运行期报告「没装离线语音」。
if(APPLE)
    target_sources(terminal_reminders PRIVATE
        cpp/voice/macos_speech_backend.cpp cpp/voice/macos_speech_backend.h)
    target_compile_definitions(terminal_reminders PUBLIC RM_TERMINAL_HAS_SPEECH=1)
elseif(UNIX)
    target_sources(terminal_reminders PRIVATE
        cpp/voice/piper_speech_backend.cpp cpp/voice/piper_speech_backend.h)
    target_compile_definitions(terminal_reminders PUBLIC RM_TERMINAL_PIPER_SPEECH=1
        RM_TERMINAL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Separate from terminal_presentation because the map pane is the first component
# that needs Qt6::Widgets; keeping Widgets off terminal_presentation stops the
# pure projection and mode-machine tests from linking a GUI stack they never use.
# 场地底图和单位图标编进二进制,不走运行时路径:操作手机器上没有源码目录,
# 而缺图会静默退化成空白地图而不是报错。AUTORCC 只对这个目标开。
add_library(terminal_map cpp/map/map_pane.cpp cpp/map/map_pane.h resources/minimap.qrc)
set_target_properties(terminal_map PROPERTIES AUTORCC ON)
target_link_libraries(terminal_map PUBLIC terminal_presentation terminal_visual Qt6::Widgets)

# Visual layer: the shared palette plus the self-drawn widgets that the reference
# layout needs (gradient roster cards). Grouped with the map because they share the
# same Widgets dependency.
add_library(terminal_visual
    cpp/ui/theme.cpp cpp/ui/theme.h
    cpp/ui/glass.cpp cpp/ui/glass.h
    cpp/ui/glass_items.cpp cpp/ui/glass_items.h
    cpp/panes/next_step_pane.cpp cpp/panes/next_step_pane.h
    cpp/ui/unit_grid.cpp cpp/ui/unit_grid.h
    cpp/panes/timeline_pane.cpp cpp/panes/timeline_pane.h
    cpp/panes/roster_pane.cpp cpp/panes/roster_pane.h
    cpp/panes/roster_panel.cpp cpp/panes/roster_panel.h
    cpp/panes/self_status_pane.cpp cpp/panes/self_status_pane.h
    cpp/game/battle_analysis.cpp cpp/game/battle_analysis.h
    cpp/panes/overview_pane.cpp cpp/panes/overview_pane.h
    cpp/panes/battle_analysis_pane.cpp cpp/panes/battle_analysis_pane.h
    cpp/panes/event_feed_pane.cpp cpp/panes/event_feed_pane.h
    cpp/ui/input_method_guard.cpp cpp/ui/input_method_guard.h
    cpp/panes/top_bar.cpp cpp/panes/top_bar.h)
target_link_libraries(terminal_visual PUBLIC terminal_presentation Qt6::Widgets Qt6::QuickWidgets)

# 角色专属面板(重装装配、指挥手飞镖/哨兵)和它们的发送侧纯逻辑。单独一个目标
# 而不是并进 terminal_visual:这是唯一需要直接链 terminal_proto 的界面层(要编码
# 三条上行指令),分开后共享面板的测试不会因为协议头改动而重编。
add_library(terminal_role_panes
    cpp/game/role_commands.cpp cpp/game/role_commands.h
    cpp/game/assembly_flow.cpp cpp/game/assembly_flow.h
    # 标记工具条要按规则 5.7.2 报出转发给哨兵的单价(sentry_mark_cost),所以跟着
    # role_commands 走,不再留在 terminal_visual 里。
    cpp/map/mark_toolbar.cpp cpp/map/mark_toolbar.h
    cpp/panes/dart_pane.cpp cpp/panes/dart_pane.h
    cpp/panes/sentry_pane.cpp cpp/panes/sentry_pane.h
    cpp/panes/self_command_pane.cpp cpp/panes/self_command_pane.h cpp/panes/self_command_view.h
    cpp/game/purchase.cpp cpp/game/purchase.h
    cpp/ui/purchase_popups.cpp cpp/ui/purchase_popups.h
    cpp/game/rune_key.cpp cpp/game/rune_key.h
    cpp/panes/hold_panels.cpp cpp/panes/hold_panels.h
    cpp/ui/revive_popup.cpp cpp/ui/revive_popup.h
    cpp/data/km_control.cpp cpp/data/km_control.h)
target_link_libraries(terminal_role_panes PUBLIC terminal_visual terminal_domain Qt6::Widgets)
target_compile_definitions(terminal_role_panes PRIVATE QT_NO_KEYWORDS)

# 切图资源。挂在**可执行文件**上而不是 terminal_visual 静态库上:塞进静态库时资源的
# 自注册符号没有任何 TU 引用它,链接器会整段丢掉,运行期表现为 qrc:/qml/TopBar.qml
# 404 而不是构建报错。每个要显示顶部栏的可执行文件都得链上 terminal_qrc。
qt_add_resources(TOPBAR_RES resources.qrc)
# 生成的 qrc_resources.cpp 只能归一个目标:多个可执行文件各自列出同一个生成文件时,并行构建会
# 同时跑几份 rcc 写同一个文件,改完 QML 后第一次构建出来的程序里可能没有资源(qrc:/qml/... 404)。
# OBJECT 库的目标文件直接链进可执行文件,不经过静态库归档,资源的自注册符号不会被链接器丢掉。
add_library(terminal_qrc OBJECT ${TOPBAR_RES})
target_link_libraries(terminal_qrc PRIVATE Qt6::Core)

# 结算逐帧素材(红胜 131 帧 + 蓝胜 132 帧,约 550MB)。刻意**不进 qrc、不进 git**:
# 塞进二进制会让产物涨三个数量级,提交进仓库则是不可逆的历史膨胀。运行时按
# game_result.cpp 里的候选路径从磁盘读,所以这里只需把它放到可执行文件旁边。
#
# 只有异常结束那 7 帧(2.2MB)随 qrc 打包 —— 与上游 resources.qrc 的取舍一致。
# 素材来自上游客户端工作树,不随本仓库分发。路径可覆盖,便于换机器或换检出位置。
set(RM26_CLIENT_DIR "$ENV{HOME}/RM26_Client" CACHE PATH
    "Upstream RM26_Client checkout that provides the victory frame sequences")
set(RESULTPANEL_SRC "${RM26_CLIENT_DIR}/resources/images/resultpanel")
if(EXISTS "${RESULTPANEL_SRC}")
    add_custom_target(result_frames ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${RESULTPANEL_SRC}/red_win_zh"
                "${CMAKE_CURRENT_BINARY_DIR}/resources/images/resultpanel/red_win_zh"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${RESULTPANEL_SRC}/blue_win_zh"
                "${CMAKE_CURRENT_BINARY_DIR}/resources/images/resultpanel/blue_win_zh"
        COMMENT "Staging victory frame sequences next to the executable")
else()
    message(STATUS
        "resultpanel frames not found at ${RESULTPANEL_SRC}; "
        "victory overlay will fall back to a static title")
endif()

# 提醒设置界面。单独一个目标而不是并进 terminal_reminders:那个库刻意不链
# Qt6::Widgets,好让调度/持久化/播放的测试不必链接一整套 GUI。设置界面是这套
# 功能里唯一需要 Widgets 的部分,所以边界划在这里。
add_library(terminal_reminder_ui
    cpp/voice/tactical_reminder_dialog.cpp cpp/voice/tactical_reminder_dialog.h)
target_link_libraries(terminal_reminder_ui PUBLIC terminal_reminders terminal_visual Qt6::Widgets)
target_compile_definitions(terminal_reminder_ui PRIVATE QT_NO_KEYWORDS)

# 开机选席位页。独立于 terminal_dashboard:它在 Dashboard 和网络接入**之前**运行,
# 反过来依赖整个主界面只会让「还没选席位」的阶段拖着一套用不上的面板。
add_library(terminal_seat_picker cpp/ui/seat_picker.cpp cpp/ui/seat_picker.h)
target_link_libraries(terminal_seat_picker PUBLIC terminal_visual terminal_logging Qt6::QuickWidgets)
target_compile_definitions(terminal_seat_picker PRIVATE QT_NO_KEYWORDS)

# A library rather than sources compiled straight into the executable, so the
# offscreen layout test can link the real Dashboard instead of a reimplementation.
add_library(terminal_dashboard cpp/ui/dashboard.cpp cpp/ui/dashboard.h
    cpp/video/video_wall.cpp cpp/video/video_wall.h
    cpp/layout/layout_editor.cpp cpp/layout/layout_editor.h
    cpp/layout/pane_details.cpp cpp/layout/pane_details.h
    cpp/ui/self_check_panel.cpp cpp/ui/self_check_panel.h
    cpp/ui/settings_panel.cpp cpp/ui/settings_panel.h
)
target_link_libraries(terminal_dashboard PUBLIC terminal_map terminal_visual terminal_video terminal_reminder_ui
    terminal_ui_profiles terminal_role_panes Qt6::Widgets)
target_compile_definitions(terminal_dashboard PRIVATE QT_NO_KEYWORDS)

add_executable(rm_terminal
    cpp/app/main.cpp
    cpp/app/platform.h
    cpp/data/mqtt_intake.cpp
    cpp/data/mqtt_intake.h
    cpp/recording/replay_player.cpp
    cpp/recording/replay_player.h
)
target_include_directories(rm_terminal PRIVATE ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(rm_terminal PRIVATE terminal_qrc terminal_dashboard terminal_recording terminal_controller
    terminal_seat_picker terminal_role_panes ${MOSQUITTO_LIBRARY} Qt6::Widgets)
target_compile_features(rm_terminal PRIVATE cxx_std_17)
target_compile_definitions(rm_terminal PRIVATE QT_NO_KEYWORDS)

if(APPLE)
    target_compile_definitions(rm_terminal PRIVATE RM_TERMINAL_PLATFORM_MACOS=1)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(rm_terminal PRIVATE RM_TERMINAL_PLATFORM_LINUX=1)
else()
    message(FATAL_ERROR "Task-1 supports macOS and Linux only")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(domain_test cpp/tests/domain_test.cpp)
    add_executable(config_test cpp/tests/config_test.cpp)
    target_link_libraries(config_test PRIVATE terminal_logging)
    add_test(NAME config_contract COMMAND config_test)
    add_executable(seat_test cpp/tests/seat_test.cpp)
    target_link_libraries(seat_test PRIVATE terminal_logging)
    add_test(NAME seat_model COMMAND seat_test)
    # 带上 qrc 并离屏运行:这个测试加载真的 SeatPicker.qml,资源不链进来就只会
    # 得到一个空 rootObject,而「QML 没加载」正是它要抓的失败之一。
    add_executable(seat_picker_test cpp/tests/seat_picker_test.cpp)
    target_link_libraries(seat_picker_test PRIVATE terminal_qrc terminal_seat_picker Qt6::Widgets Qt6::QuickWidgets)
    target_compile_definitions(seat_picker_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME seat_picker COMMAND seat_picker_test)
    set_tests_properties(seat_picker PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_executable(intake_test cpp/tests/intake_test.cpp)
    add_executable(video_test cpp/tests/video_test.cpp)
    target_link_libraries(video_test PRIVATE terminal_video)
    add_test(NAME video_protocol COMMAND video_test)
    add_executable(video_recovery_test cpp/tests/video_recovery_test.cpp)
    target_link_libraries(video_recovery_test PRIVATE terminal_video)
    add_test(NAME video_recovery_policy COMMAND video_recovery_test)
    add_executable(recorder_test cpp/tests/recorder_test.cpp)
    target_link_libraries(recorder_test PRIVATE terminal_recording)
    add_test(NAME recording_format COMMAND recorder_test)
    add_executable(referee_frame_test cpp/tests/referee_frame_test.cpp)
    target_link_libraries(referee_frame_test PRIVATE terminal_controller)
    add_test(NAME referee_frame COMMAND referee_frame_test)
    add_executable(controller_link_test cpp/tests/controller_link_test.cpp)
    target_link_libraries(controller_link_test PRIVATE terminal_controller)
    add_test(NAME controller_link COMMAND controller_link_test)
    set_tests_properties(controller_link PROPERTIES TIMEOUT 30)
    add_executable(link_health_test cpp/tests/link_health_test.cpp)
    target_link_libraries(link_health_test PRIVATE terminal_presentation)
    add_test(NAME link_health COMMAND link_health_test)
    add_executable(match_alerts_test cpp/tests/match_alerts_test.cpp)
    target_link_libraries(match_alerts_test PRIVATE terminal_presentation)
    add_test(NAME match_alerts COMMAND match_alerts_test)
    add_executable(next_step_test cpp/tests/next_step_test.cpp)
    target_link_libraries(next_step_test PRIVATE terminal_presentation)
    add_test(NAME next_step COMMAND next_step_test)
    add_executable(buff_ledger_test cpp/tests/buff_ledger_test.cpp)
    target_link_libraries(buff_ledger_test PRIVATE terminal_presentation)
    add_test(NAME buff_ledger COMMAND buff_ledger_test)
    add_executable(map_effects_test cpp/tests/map_effects_test.cpp)
    target_link_libraries(map_effects_test PRIVATE terminal_presentation)
    add_test(NAME map_effects COMMAND map_effects_test)
    add_executable(overview_test cpp/tests/overview_test.cpp)
    target_link_libraries(overview_test PRIVATE terminal_presentation)
    add_test(NAME overview COMMAND overview_test)
    add_executable(radar_intel_test cpp/tests/radar_intel_test.cpp)
    target_link_libraries(radar_intel_test PRIVATE terminal_visual)
    add_test(NAME radar_intel COMMAND radar_intel_test)
    add_executable(replay_player_test cpp/tests/replay_player_test.cpp cpp/recording/replay_player.cpp cpp/recording/replay_player.h)
    target_link_libraries(replay_player_test PRIVATE terminal_domain terminal_recording terminal_video Qt6::Core)
    target_compile_definitions(replay_player_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME replay_player COMMAND replay_player_test)
    add_executable(video_diagnostic cpp/video/video_diagnostic.cpp)
    target_link_libraries(video_diagnostic PRIVATE terminal_video Qt6::Core Qt6::Network)
    target_link_libraries(intake_test PRIVATE terminal_domain terminal_proto)
    add_test(NAME intake_contract COMMAND intake_test)
    target_link_libraries(domain_test PRIVATE terminal_visual)
    add_test(NAME domain_state COMMAND domain_test)
    add_executable(ui_mode_test cpp/tests/ui_mode_test.cpp)
    target_link_libraries(ui_mode_test PRIVATE terminal_presentation)
    add_test(NAME ui_mode_state COMMAND ui_mode_test)
    add_executable(popup_state_test cpp/tests/popup_state_test.cpp)
    target_link_libraries(popup_state_test PRIVATE terminal_presentation)
    add_test(NAME popup_state COMMAND popup_state_test)
    # 带上 qrc:异常终止那 7 帧是唯一打进二进制的序列,不链接资源的话
    # qrc 分支就永远是死代码,注册漏写要到运行期才暴露。
    add_executable(game_result_test cpp/tests/game_result_test.cpp)
    target_link_libraries(game_result_test PRIVATE terminal_qrc terminal_presentation)
    add_test(NAME game_result COMMAND game_result_test)
    add_executable(field_map_test cpp/tests/field_map_test.cpp)
    target_link_libraries(field_map_test PRIVATE terminal_presentation)
    add_test(NAME field_map_projection COMMAND field_map_test)
    add_executable(battle_analysis_test cpp/tests/battle_analysis_test.cpp)
    target_link_libraries(battle_analysis_test PRIVATE terminal_visual)
    add_test(NAME battle_analysis_metrics COMMAND battle_analysis_test)
    # 纯逻辑:编码逐字段往返、限频、按住确认、回显状态机。不链 Widgets。
    add_executable(role_commands_test cpp/tests/role_commands_test.cpp)
    target_link_libraries(role_commands_test PRIVATE terminal_role_panes)
    target_compile_definitions(role_commands_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME role_commands COMMAND role_commands_test)
    # 离屏运行:断线禁用和「一次点击只发一条」要在真实 QPushButton 上验证。
    add_executable(role_panes_test cpp/tests/role_panes_test.cpp)
    target_link_libraries(role_panes_test PRIVATE terminal_role_panes Qt6::Widgets)
    target_compile_definitions(role_panes_test PRIVATE QT_NO_KEYWORDS
        RM_TERMINAL_ARTIFACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/artifacts")
    add_test(NAME role_panes COMMAND role_panes_test)
    set_tests_properties(role_panes PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    # 兑换:条件和原因、价钱、到没到账是纯逻辑;H 远程兑换框、O/I 补弹面板、确认框用离屏窗口验。
    add_executable(purchase_test cpp/tests/purchase_test.cpp)
    target_link_libraries(purchase_test PRIVATE terminal_role_panes Qt6::Widgets)
    target_compile_definitions(purchase_test PRIVATE QT_NO_KEYWORDS
        RM_TERMINAL_ARTIFACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/artifacts")
    add_test(NAME purchase COMMAND purchase_test)
    set_tests_properties(purchase PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    # 重装装配:选难度、开始、确认、退出和进度条是纯逻辑;H 框和进度条用离屏窗口截图。
    add_executable(assembly_flow_test cpp/tests/assembly_flow_test.cpp)
    target_link_libraries(assembly_flow_test PRIVATE terminal_role_panes Qt6::Widgets)
    target_compile_definitions(assembly_flow_test PRIVATE QT_NO_KEYWORDS
        RM_TERMINAL_ARTIFACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/artifacts")
    add_test(NAME assembly_flow COMMAND assembly_flow_test)
    set_tests_properties(assembly_flow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    # P 设置面板:终端设置的存盘、面板上的每一项、输入法只给打字框。
    add_executable(settings_panel_test cpp/tests/settings_panel_test.cpp)
    target_link_libraries(settings_panel_test PRIVATE terminal_qrc terminal_dashboard Qt6::Widgets Qt6::QuickWidgets)
    target_compile_definitions(settings_panel_test PRIVATE QT_NO_KEYWORDS
        RM_TERMINAL_ARTIFACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/artifacts")
    add_test(NAME settings_panel COMMAND settings_panel_test)
    set_tests_properties(settings_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    # 按住的键:长按 F 开能量机关(次数推算、按满一秒才发)、按住 Tab / ~ 的面板。
    add_executable(hold_keys_test cpp/tests/hold_keys_test.cpp)
    target_link_libraries(hold_keys_test PRIVATE terminal_role_panes Qt6::Widgets)
    target_compile_definitions(hold_keys_test PRIVATE QT_NO_KEYWORDS
        RM_TERMINAL_ARTIFACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/artifacts")
    add_test(NAME hold_keys COMMAND hold_keys_test)
    set_tests_properties(hold_keys PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    # 键鼠控制:键位映射、累积和 75Hz 节拍是纯逻辑;接管窗口那层用离屏窗口和合成事件验。
    add_executable(km_control_test cpp/tests/km_control_test.cpp)
    target_link_libraries(km_control_test PRIVATE terminal_role_panes Qt6::Widgets)
    target_compile_definitions(km_control_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME km_control COMMAND km_control_test)
    set_tests_properties(km_control PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    # 真实 say/afplay 子进程测试,只在 macOS 注册:它断言的是本机语音后端,
    # 在没有这些工具的平台上注册只会得到必然失败的红灯。
    if(APPLE)
        add_executable(reminder_audio_test cpp/tests/reminder_audio_test.cpp)
        target_link_libraries(reminder_audio_test PRIVATE terminal_reminders)
        add_test(NAME reminder_audio COMMAND reminder_audio_test)
        set_tests_properties(reminder_audio PROPERTIES TIMEOUT 120)
    endif()
    add_executable(reminder_playback_test cpp/tests/reminder_playback_test.cpp)
    target_link_libraries(reminder_playback_test PRIVATE terminal_reminders)
    add_test(NAME reminder_playback COMMAND reminder_playback_test)
    add_executable(reminder_repository_test cpp/tests/reminder_repository_test.cpp)
    target_link_libraries(reminder_repository_test PRIVATE terminal_reminders)
    add_test(NAME reminder_repository COMMAND reminder_repository_test)
    add_executable(ui_profile_test cpp/tests/ui_profile_test.cpp)
    target_link_libraries(ui_profile_test PRIVATE terminal_ui_profiles)
    target_compile_definitions(ui_profile_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME ui_profile COMMAND ui_profile_test)
    add_executable(reminder_scheduler_test cpp/tests/reminder_scheduler_test.cpp)
    target_link_libraries(reminder_scheduler_test PRIVATE terminal_presentation)
    add_test(NAME reminder_scheduler COMMAND reminder_scheduler_test)
    add_executable(reminder_controller_test cpp/tests/tactical_reminder_controller_test.cpp)
    target_link_libraries(reminder_controller_test PRIVATE terminal_reminders)
    add_test(NAME reminder_controller COMMAND reminder_controller_test)
    # 离屏运行:设置界面的锁定和草稿行为要在真实 QWidget 上验证,但 CI 没有显示器。
    add_executable(reminder_dialog_test cpp/tests/tactical_reminder_dialog_test.cpp)
    target_link_libraries(reminder_dialog_test PRIVATE terminal_reminder_ui Qt6::Widgets)
    target_compile_definitions(reminder_dialog_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME reminder_dialog COMMAND reminder_dialog_test)
    set_tests_properties(reminder_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_executable(top_bar_model_test cpp/tests/top_bar_model_test.cpp)
    target_link_libraries(top_bar_model_test PRIVATE terminal_presentation)
    add_test(NAME top_bar_model COMMAND top_bar_model_test)
    # 资源随可执行文件带:这个测试构造真的 Dashboard,顶部栏要能真的加载 QML。
    add_executable(dashboard_layout_test cpp/tests/dashboard_layout_test.cpp)
    add_executable(roster_runtime_test cpp/tests/roster_runtime_test.cpp)
    target_link_libraries(roster_runtime_test PRIVATE terminal_qrc terminal_visual Qt6::QuickWidgets)
    add_test(NAME roster_runtime COMMAND roster_runtime_test)
    set_tests_properties(roster_runtime PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    target_link_libraries(dashboard_layout_test PRIVATE terminal_qrc terminal_dashboard Qt6::Widgets Qt6::QuickWidgets)
    target_compile_definitions(dashboard_layout_test PRIVATE
        QT_NO_KEYWORDS
        RM_TERMINAL_DASHBOARD_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/cpp/ui/dashboard.cpp")
    add_test(NAME dashboard_layout COMMAND dashboard_layout_test)
    set_tests_properties(dashboard_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    # 指挥手的图传墙:排格子、每格写什么、画出来对不对,以及地面席位的转发原样到达。
    add_executable(video_wall_test cpp/tests/video_wall_test.cpp)
    target_link_libraries(video_wall_test PRIVATE terminal_qrc terminal_dashboard Qt6::Widgets)
    target_compile_definitions(video_wall_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME video_wall COMMAND video_wall_test)
    set_tests_properties(video_wall PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_executable(layout_editor_test cpp/tests/layout_editor_test.cpp)
    target_link_libraries(layout_editor_test PRIVATE terminal_qrc terminal_dashboard Qt6::Widgets)
    target_compile_definitions(layout_editor_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME layout_editor COMMAND layout_editor_test)
    set_tests_properties(layout_editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

    # 验收用截图工具,不注册为测试:它只产出图片,不做断言。
    add_executable(popup_shots cpp/tools/popup_shots.cpp)
    target_link_libraries(popup_shots PRIVATE terminal_qrc terminal_dashboard Qt6::Widgets)
    target_compile_definitions(popup_shots PRIVATE QT_NO_KEYWORDS)

    # 结算动画验收工具,同样不注册为测试:红胜/蓝胜素材按设计不进仓库,
    # 缺素材的机器上它只会退回静态标题,不该因此让 ctest 变红。
    add_executable(result_shots cpp/tools/result_shots.cpp)
    target_link_libraries(result_shots PRIVATE terminal_qrc terminal_dashboard Qt6::Widgets)
    target_compile_definitions(result_shots PRIVATE QT_NO_KEYWORDS)

    # 选席位页出图工具,同样不注册为测试:它产出 picker.png 这张验收图,不做断言。
    add_executable(picker_shot cpp/tools/picker_shot.cpp)
    target_link_libraries(picker_shot PRIVATE terminal_qrc terminal_seat_picker Qt6::Widgets)
    target_compile_definitions(picker_shot PRIVATE QT_NO_KEYWORDS)

    add_executable(map_pane_test cpp/tests/map_pane_test.cpp)
    target_link_libraries(map_pane_test PRIVATE terminal_map Qt6::Widgets)
    add_test(NAME map_pane_render COMMAND map_pane_test)
    add_executable(map_marks_test cpp/tests/map_marks_test.cpp)
    target_link_libraries(map_marks_test PRIVATE terminal_presentation)
    add_test(NAME map_marks COMMAND map_marks_test)
    # Offscreen so the pixel assertions run identically on a headless Ubuntu box
    # and on a macOS workstation with a real display attached.
    set_tests_properties(map_pane_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_executable(clock_test cpp/tests/clock_test.cpp)
    target_link_libraries(clock_test PRIVATE terminal_domain)
    add_test(NAME monotonic_clock COMMAND clock_test)
    add_test(NAME monotonic_clock_guard COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/assert_monotonic_clock.cmake)
    add_test(NAME reject_unknown_args COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:rm_terminal> -P ${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/assert_bad_args.cmake)
    add_test(NAME safe_start COMMAND rm_terminal --safe-smoke)
    set_tests_properties(safe_start domain_state reject_unknown_args PROPERTIES TIMEOUT 15)
    # Simulator-backed end-to-end timing check. It starts its own broker, match
    # server and video sender, and needs ~65s of wall clock because it must observe
    # a 10s blind window plus the 3s exit hysteresis.
    #
    # Registration is opt-in because a CTest label cannot keep this out of a plain
    # `ctest` run: -L selects and -LE excludes, but neither makes a registered test
    # skip by default, so registering it unconditionally would take the default
    # suite from seconds to over a minute. Off by default keeps the edit-test loop
    # fast; turn it on for the integration run:
    #   cmake -S . -B build/macos-integration -DRM_TERMINAL_INTEGRATION_TESTS=ON
    #   ctest --test-dir build/macos-integration -L integration --output-on-failure
    # The script is also runnable directly: bash scripts/verify_mode_regression.sh
    option(RM_TERMINAL_INTEGRATION_TESTS
           "Register simulator-backed integration tests (adds ~65s to ctest)" OFF)
    if(RM_TERMINAL_INTEGRATION_TESTS)
        add_test(NAME mode_regression_e2e
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_mode_regression.sh)
        set_tests_properties(mode_regression_e2e PROPERTIES
            LABELS integration
            TIMEOUT 180
            RUN_SERIAL TRUE
            ENVIRONMENT "RM_TERMINAL_BINARY=$<TARGET_FILE:rm_terminal>")
        # 链路故障验收:逐条断开裁判数据/车端/图传/机械臂,约 2 分钟。
        add_test(NAME link_faults_e2e
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python
                         ${CMAKE_CURRENT_SOURCE_DIR}/scripts/accept_links.py)
        set_tests_properties(link_faults_e2e PROPERTIES
            LABELS integration
            TIMEOUT 300
            RUN_SERIAL TRUE
            ENVIRONMENT "RM_TERMINAL_BINARY=$<TARGET_FILE:rm_terminal>")
    endif()
endif()
