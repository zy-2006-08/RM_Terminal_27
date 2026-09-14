foreach(case IN ITEMS unknown extra duplicate positional empty)
    # Given: malformed command-line arguments.
    if(case STREQUAL "unknown")
        set(args --unknown)
    elseif(case STREQUAL "extra")
        set(args --safe-smoke extra)
    elseif(case STREQUAL "duplicate")
        set(args --safe-smoke --safe-smoke)
    elseif(case STREQUAL "positional")
        set(args extra)
    else()
        set(args "")
    endif()
    # When: invoke the real binary with a hard execution bound.
    if(case STREQUAL "empty")
        execute_process(COMMAND "${PROGRAM}" "" RESULT_VARIABLE result
            OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 2)
    else()
        execute_process(COMMAND "${PROGRAM}" ${args} RESULT_VARIABLE result
            OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 2)
    endif()
    # Then: normal error exit, no success output, and a bounded diagnostic.
    string(LENGTH "${error}" error_length)
    if(NOT result STREQUAL "2" OR NOT output STREQUAL "" OR
       error_length EQUAL 0 OR error_length GREATER 256 OR error MATCHES "SIMULATION SAFE")
        message(FATAL_ERROR "${case}: exit=${result}; stdout=${output}; stderr=${error}")
    endif()
    message(STATUS "${case}: exit=${result}; stdout empty; stderr=${error}")
endforeach()
