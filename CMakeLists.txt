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

add_library(terminal_logging cpp/logging.cpp cpp/logging.h cpp/config.cpp cpp/config.h)
target_include_directories(terminal_logging PUBLIC cpp)
target_link_libraries(terminal_logging PUBLIC Qt6::Core)

add_library(terminal_domain cpp/conversion.cpp cpp/store.cpp cpp/decoder.cpp cpp/clock.cpp cpp/clock.h)
target_include_directories(terminal_domain PUBLIC cpp)
target_link_libraries(terminal_domain PUBLIC terminal_proto)
add_library(terminal_video cpp/video.cpp cpp/video_receiver.cpp cpp/video_receiver.h cpp/video_recovery.cpp cpp/video_recovery.h)
target_include_directories(terminal_video PUBLIC cpp)
target_link_libraries(terminal_video PUBLIC terminal_logging Qt6::Core Qt6::Network)
add_library(terminal_presentation cpp/presentation.cpp cpp/ui_mode.cpp cpp/ui_mode.h cpp/popup_state.cpp cpp/popup_state.h cpp/game_result.cpp cpp/game_result.h cpp/field_map.cpp cpp/field_map.h cpp/top_bar_model.cpp cpp/top_bar_model.h cpp/reminder_scheduler.cpp cpp/reminder_scheduler.h cpp/tactical_reminder.h)
target_link_libraries(terminal_presentation PUBLIC terminal_domain terminal_logging Qt6::Core)

# 战术提醒的持久化和语音后端。独立于 terminal_presentation:调度器是纯逻辑,
# 而这一层要碰文件系统和子进程,分开后纯逻辑测试不必链接这些副作用。
add_library(terminal_reminders
    cpp/reminder_repository.cpp cpp/reminder_repository.h
    cpp/reminder_playback.cpp cpp/reminder_playback.h
    cpp/tactical_reminder_controller.cpp cpp/tactical_reminder_controller.h
    cpp/speech_backend.h)
target_include_directories(terminal_reminders PUBLIC cpp)
target_link_libraries(terminal_reminders PUBLIC terminal_presentation Qt6::Core)
# 语音后端按平台条件编译。Linux 上刻意没有后端:没有已核实的本机中文离线语音,
# 与其接一个念不出中文的实现,不如让上层明确报告该平台没有语音。
if(APPLE)
    target_sources(terminal_reminders PRIVATE
        cpp/macos_speech_backend.cpp cpp/macos_speech_backend.h)
    target_compile_definitions(terminal_reminders PUBLIC RM_TERMINAL_HAS_SPEECH=1)
endif()

# Separate from terminal_presentation because the map pane is the first component
# that needs Qt6::Widgets; keeping Widgets off terminal_presentation stops the
# pure projection and mode-machine tests from linking a GUI stack they never use.
# 场地底图和单位图标编进二进制,不走运行时路径:操作手机器上没有源码目录,
# 而缺图会静默退化成空白地图而不是报错。AUTORCC 只对这个目标开。
add_library(terminal_map cpp/map_pane.cpp cpp/map_pane.h assets/minimap.qrc)
set_target_properties(terminal_map PROPERTIES AUTORCC ON)
target_link_libraries(terminal_map PUBLIC terminal_presentation Qt6::Widgets)

# Visual layer: the shared palette plus the self-drawn widgets that the reference
# layout needs (gradient roster cards). Grouped with the map because they share the
# same Widgets dependency.
add_library(terminal_visual
    cpp/theme.cpp cpp/theme.h
    cpp/roster_pane.cpp cpp/roster_pane.h
    cpp/roster_panel.cpp cpp/roster_panel.h
    cpp/self_status_pane.cpp cpp/self_status_pane.h
    cpp/battle_analysis.cpp cpp/battle_analysis.h
    cpp/battle_analysis_pane.cpp cpp/battle_analysis_pane.h
    cpp/event_feed_pane.cpp cpp/event_feed_pane.h
    cpp/top_bar.cpp cpp/top_bar.h)
target_link_libraries(terminal_visual PUBLIC terminal_presentation Qt6::Widgets Qt6::QuickWidgets)

# 切图资源。挂在**可执行文件**上而不是 terminal_visual 静态库上:塞进静态库时资源的
# 自注册符号没有任何 TU 引用它,链接器会整段丢掉,运行期表现为 qrc:/qml/TopBar.qml
# 404 而不是构建报错。每个要显示顶部栏的可执行文件都得带上它。
qt_add_resources(TOPBAR_RES resources.qrc)

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
    cpp/tactical_reminder_dialog.cpp cpp/tactical_reminder_dialog.h)
target_link_libraries(terminal_reminder_ui PUBLIC terminal_reminders terminal_visual Qt6::Widgets)
target_compile_definitions(terminal_reminder_ui PRIVATE QT_NO_KEYWORDS)

# A library rather than sources compiled straight into the executable, so the
# offscreen layout test can link the real Dashboard instead of a reimplementation.
add_library(terminal_dashboard cpp/dashboard.cpp cpp/dashboard.h)
target_link_libraries(terminal_dashboard PUBLIC terminal_map terminal_visual terminal_video terminal_reminder_ui Qt6::Widgets)
target_compile_definitions(terminal_dashboard PRIVATE QT_NO_KEYWORDS)

add_executable(rm_terminal
    cpp/main.cpp
    cpp/platform.h
    cpp/mqtt_intake.cpp
    cpp/mqtt_intake.h
    ${TOPBAR_RES}
)
target_include_directories(rm_terminal PRIVATE ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(rm_terminal PRIVATE terminal_dashboard ${MOSQUITTO_LIBRARY} Qt6::Widgets)
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
    add_executable(domain_test cpp/domain_test.cpp)
    add_executable(config_test cpp/config_test.cpp)
    target_link_libraries(config_test PRIVATE terminal_logging)
    add_test(NAME config_contract COMMAND config_test)
    add_executable(intake_test cpp/intake_test.cpp)
    add_executable(video_test cpp/video_test.cpp)
    target_link_libraries(video_test PRIVATE terminal_video)
    add_test(NAME video_protocol COMMAND video_test)
    add_executable(video_recovery_test cpp/video_recovery_test.cpp)
    target_link_libraries(video_recovery_test PRIVATE terminal_video)
    add_test(NAME video_recovery_policy COMMAND video_recovery_test)
    add_executable(video_diagnostic cpp/video_diagnostic.cpp)
    target_link_libraries(video_diagnostic PRIVATE terminal_video Qt6::Core Qt6::Network)
    target_link_libraries(intake_test PRIVATE terminal_domain terminal_proto)
    add_test(NAME intake_contract COMMAND intake_test)
    target_link_libraries(domain_test PRIVATE terminal_visual)
    add_test(NAME domain_state COMMAND domain_test)
    add_executable(ui_mode_test cpp/ui_mode_test.cpp)
    target_link_libraries(ui_mode_test PRIVATE terminal_presentation)
    add_test(NAME ui_mode_state COMMAND ui_mode_test)
    add_executable(popup_state_test cpp/popup_state_test.cpp)
    target_link_libraries(popup_state_test PRIVATE terminal_presentation)
    add_test(NAME popup_state COMMAND popup_state_test)
    # 带上 qrc:异常终止那 7 帧是唯一打进二进制的序列,不链接资源的话
    # qrc 分支就永远是死代码,注册漏写要到运行期才暴露。
    add_executable(game_result_test cpp/game_result_test.cpp ${TOPBAR_RES})
    target_link_libraries(game_result_test PRIVATE terminal_presentation)
    add_test(NAME game_result COMMAND game_result_test)
    add_executable(field_map_test cpp/field_map_test.cpp)
    target_link_libraries(field_map_test PRIVATE terminal_presentation)
    add_test(NAME field_map_projection COMMAND field_map_test)
    add_executable(battle_analysis_test cpp/battle_analysis_test.cpp)
    target_link_libraries(battle_analysis_test PRIVATE terminal_visual)
    add_test(NAME battle_analysis_metrics COMMAND battle_analysis_test)
    # 真实 say/afplay 子进程测试,只在 macOS 注册:它断言的是本机语音后端,
    # 在没有这些工具的平台上注册只会得到必然失败的红灯。
    if(APPLE)
        add_executable(reminder_audio_test cpp/reminder_audio_test.cpp)
        target_link_libraries(reminder_audio_test PRIVATE terminal_reminders)
        add_test(NAME reminder_audio COMMAND reminder_audio_test)
        set_tests_properties(reminder_audio PROPERTIES TIMEOUT 120)
    endif()
    add_executable(reminder_playback_test cpp/reminder_playback_test.cpp)
    target_link_libraries(reminder_playback_test PRIVATE terminal_reminders)
    add_test(NAME reminder_playback COMMAND reminder_playback_test)
    add_executable(reminder_repository_test cpp/reminder_repository_test.cpp)
    target_link_libraries(reminder_repository_test PRIVATE terminal_reminders)
    add_test(NAME reminder_repository COMMAND reminder_repository_test)
    add_executable(reminder_scheduler_test cpp/reminder_scheduler_test.cpp)
    target_link_libraries(reminder_scheduler_test PRIVATE terminal_presentation)
    add_test(NAME reminder_scheduler COMMAND reminder_scheduler_test)
    add_executable(reminder_controller_test cpp/tactical_reminder_controller_test.cpp)
    target_link_libraries(reminder_controller_test PRIVATE terminal_reminders)
    add_test(NAME reminder_controller COMMAND reminder_controller_test)
    # 离屏运行:设置界面的锁定和草稿行为要在真实 QWidget 上验证,但 CI 没有显示器。
    add_executable(reminder_dialog_test cpp/tactical_reminder_dialog_test.cpp)
    target_link_libraries(reminder_dialog_test PRIVATE terminal_reminder_ui Qt6::Widgets)
    target_compile_definitions(reminder_dialog_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME reminder_dialog COMMAND reminder_dialog_test)
    set_tests_properties(reminder_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_executable(top_bar_model_test cpp/top_bar_model_test.cpp)
    target_link_libraries(top_bar_model_test PRIVATE terminal_presentation)
    add_test(NAME top_bar_model COMMAND top_bar_model_test)
    # 资源随可执行文件带:这个测试构造真的 Dashboard,顶部栏要能真的加载 QML。
    add_executable(dashboard_layout_test cpp/dashboard_layout_test.cpp ${TOPBAR_RES})
    target_link_libraries(dashboard_layout_test PRIVATE terminal_dashboard Qt6::Widgets Qt6::QuickWidgets)
    target_compile_definitions(dashboard_layout_test PRIVATE
        QT_NO_KEYWORDS
        RM_TERMINAL_DASHBOARD_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/cpp/dashboard.cpp")
    add_test(NAME dashboard_layout COMMAND dashboard_layout_test)
    set_tests_properties(dashboard_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

    # 验收用截图工具,不注册为测试:它只产出图片,不做断言。
    add_executable(popup_shots cpp/popup_shots.cpp ${TOPBAR_RES})
    target_link_libraries(popup_shots PRIVATE terminal_dashboard Qt6::Widgets)
    target_compile_definitions(popup_shots PRIVATE QT_NO_KEYWORDS)

    # 结算动画验收工具,同样不注册为测试:红胜/蓝胜素材按设计不进仓库,
    # 缺素材的机器上它只会退回静态标题,不该因此让 ctest 变红。
    add_executable(result_shots cpp/result_shots.cpp ${TOPBAR_RES})
    target_link_libraries(result_shots PRIVATE terminal_dashboard Qt6::Widgets)
    target_compile_definitions(result_shots PRIVATE QT_NO_KEYWORDS)

    add_executable(map_pane_test cpp/map_pane_test.cpp)
    target_link_libraries(map_pane_test PRIVATE terminal_map Qt6::Widgets)
    add_test(NAME map_pane_render COMMAND map_pane_test)
    # Offscreen so the pixel assertions run identically on a headless Ubuntu box
    # and on a macOS workstation with a real display attached.
    set_tests_properties(map_pane_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_executable(clock_test cpp/clock_test.cpp)
    target_link_libraries(clock_test PRIVATE terminal_domain)
    add_test(NAME monotonic_clock COMMAND clock_test)
    add_test(NAME monotonic_clock_guard COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cpp/assert_monotonic_clock.cmake)
    add_test(NAME reject_unknown_args COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:rm_terminal> -P ${CMAKE_CURRENT_SOURCE_DIR}/cpp/assert_bad_args.cmake)
    add_test(NAME safe_start COMMAND rm_terminal --safe-smoke)
    set_tests_properties(safe_start domain_state reject_unknown_args PROPERTIES TIMEOUT 15)
    # Simulator-backed end-to-end timing check. It starts its own broker, match
    # server and video sender, and needs ~65s of wall clock because it must observe
    # a 12s blind window plus the 3s exit hysteresis.
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
    endif()
endif()
