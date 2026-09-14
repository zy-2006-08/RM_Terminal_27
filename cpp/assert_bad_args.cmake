# A path the binary must never write: every case below is rejected during argument
# parsing, which happens before any file is opened.
set(REJECTED_DUMP "${CMAKE_CURRENT_BINARY_DIR}/rejected-dump-layout.json")
file(REMOVE "${REJECTED_DUMP}" "${REJECTED_DUMP}2")

foreach(case IN ITEMS unknown extra duplicate positional empty
                     force_mode_missing force_mode_invalid force_mode_duplicate
                     force_mode_with_safe_smoke force_mode_with_diagnostic
                     dump_layout_missing dump_layout_duplicate
                     dump_layout_with_safe_smoke dump_layout_with_diagnostic)
    # Given: malformed command-line arguments, plus the diagnostic each one must
    # produce. Exit code 2 alone is too weak an assertion: a flag-specific guard can
    # be deleted and the leftover token still exits 2 via the unknown-argument
    # fallback, so every case pins the specific message it expects.
    if(case STREQUAL "unknown")
        set(args --unknown)
        set(expect "Unknown argument; supported:")
    elseif(case STREQUAL "extra")
        set(args --safe-smoke extra)
        set(expect "Unknown argument; supported:")
    elseif(case STREQUAL "duplicate")
        set(args --safe-smoke --safe-smoke)
        set(expect "Unknown argument; supported:")
    elseif(case STREQUAL "positional")
        set(args extra)
        set(expect "Unknown argument; supported:")
    elseif(case STREQUAL "force_mode_missing")
        set(args --force-mode)
        set(expect "Missing mode after --force-mode")
    elseif(case STREQUAL "force_mode_invalid")
        set(args --force-mode bogus)
        set(expect "Invalid --force-mode value")
    elseif(case STREQUAL "force_mode_duplicate")
        set(args --force-mode info --force-mode video)
        set(expect "Duplicate --force-mode is not supported")
    # --force-mode forces a GUI layout, so pairing it with a headless mode is
    # rejected rather than ignored: silence would let an evidence-capture command
    # look successful while proving nothing.
    elseif(case STREQUAL "force_mode_with_safe_smoke")
        set(args --safe-smoke --force-mode info)
        set(expect "--force-mode requires the GUI")
    elseif(case STREQUAL "force_mode_with_diagnostic")
        set(args --diagnostic localhost 1883 2 --force-mode video)
        set(expect "--force-mode requires the GUI")
    elseif(case STREQUAL "dump_layout_missing")
        set(args --dump-layout)
        set(expect "Missing path after --dump-layout")
    elseif(case STREQUAL "dump_layout_duplicate")
        set(args --dump-layout "${REJECTED_DUMP}" --dump-layout "${REJECTED_DUMP}2")
        set(expect "Duplicate --dump-layout is not supported")
    # --dump-layout describes a GUI layout, so the headless modes are rejected for the
    # same reason as --force-mode: a silently ignored flag writes no evidence while
    # still exiting 0.
    elseif(case STREQUAL "dump_layout_with_safe_smoke")
        set(args --safe-smoke --dump-layout "${REJECTED_DUMP}")
        set(expect "--dump-layout requires the GUI")
    elseif(case STREQUAL "dump_layout_with_diagnostic")
        set(args --diagnostic localhost 1883 2 --dump-layout "${REJECTED_DUMP}")
        set(expect "--dump-layout requires the GUI")
    else()
        set(args "")
        set(expect "Unknown argument; supported:")
    endif()
    # When: invoke the real binary with a hard execution bound.
    if(case STREQUAL "empty")
        execute_process(COMMAND "${PROGRAM}" "" RESULT_VARIABLE result
            OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 2)
    else()
        execute_process(COMMAND "${PROGRAM}" ${args} RESULT_VARIABLE result
            OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 2)
    endif()
    # Then: normal error exit, no success output, and the expected bounded diagnostic.
    string(LENGTH "${error}" error_length)
    string(FIND "${error}" "${expect}" expect_at)
    if(NOT result STREQUAL "2" OR NOT output STREQUAL "" OR
       error_length EQUAL 0 OR error_length GREATER 256 OR error MATCHES "SIMULATION SAFE")
        message(FATAL_ERROR "${case}: exit=${result}; stdout=${output}; stderr=${error}")
    endif()
    if(expect_at EQUAL -1)
        message(FATAL_ERROR "${case}: expected stderr to contain '${expect}'; got '${error}'")
    endif()
    message(STATUS "${case}: exit=${result}; stdout empty; stderr=${error}")
endforeach()

if(EXISTS "${REJECTED_DUMP}" OR EXISTS "${REJECTED_DUMP}2")
    message(FATAL_ERROR "a rejected --dump-layout invocation still wrote ${REJECTED_DUMP}")
endif()
message(STATUS "rejected --dump-layout paths were never written")
