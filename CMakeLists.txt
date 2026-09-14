cmake_minimum_required(VERSION 3.21)
project(rm_terminal VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
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

add_library(terminal_domain cpp/conversion.cpp cpp/store.cpp cpp/decoder.cpp)
target_include_directories(terminal_domain PUBLIC cpp)
target_link_libraries(terminal_domain PUBLIC terminal_proto)
add_library(terminal_video cpp/video.cpp cpp/video_receiver.cpp cpp/video_receiver.h)
target_include_directories(terminal_video PUBLIC cpp)
target_link_libraries(terminal_video PUBLIC terminal_logging Qt6::Core Qt6::Network)
add_library(terminal_presentation cpp/presentation.cpp cpp/ui_mode.cpp cpp/ui_mode.h cpp/field_map.cpp cpp/field_map.h)
target_link_libraries(terminal_presentation PUBLIC terminal_domain terminal_logging Qt6::Core)

add_executable(rm_terminal
    cpp/main.cpp
    cpp/platform.h
    cpp/mqtt_intake.cpp
    cpp/mqtt_intake.h
    cpp/dashboard.cpp
    cpp/dashboard.h
)
target_include_directories(rm_terminal PRIVATE ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(rm_terminal PRIVATE terminal_presentation terminal_video ${MOSQUITTO_LIBRARY} Qt6::Widgets)
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
    add_executable(video_diagnostic cpp/video_diagnostic.cpp)
    target_link_libraries(video_diagnostic PRIVATE terminal_video Qt6::Core Qt6::Network)
    target_link_libraries(intake_test PRIVATE terminal_domain terminal_proto)
    add_test(NAME intake_contract COMMAND intake_test)
    target_link_libraries(domain_test PRIVATE terminal_presentation)
    add_test(NAME domain_state COMMAND domain_test)
    add_executable(ui_mode_test cpp/ui_mode_test.cpp)
    target_link_libraries(ui_mode_test PRIVATE terminal_presentation)
    add_test(NAME ui_mode_state COMMAND ui_mode_test)
    add_executable(field_map_test cpp/field_map_test.cpp)
    target_link_libraries(field_map_test PRIVATE terminal_presentation)
    add_test(NAME field_map_projection COMMAND field_map_test)
    add_test(NAME reject_unknown_args COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:rm_terminal> -P ${CMAKE_CURRENT_SOURCE_DIR}/cpp/assert_bad_args.cmake)
    add_test(NAME safe_start COMMAND rm_terminal --safe-smoke)
    set_tests_properties(safe_start domain_state reject_unknown_args PROPERTIES TIMEOUT 15)
endif()
