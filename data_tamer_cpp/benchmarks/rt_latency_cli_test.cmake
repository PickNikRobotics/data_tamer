function(expect_rejected)
    execute_process(
        COMMAND "${RT_LATENCY}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_QUIET
        ERROR_VARIABLE error
        TIMEOUT 2)
    if(NOT result STREQUAL "1" OR NOT error MATCHES "invalid option")
        message(FATAL_ERROR "${ARGN}: result=${result}, stderr=${error}")
    endif()
endfunction()

expect_rejected(--values)
expect_rejected(--seconds 0)
expect_rejected(--writers -1)
expect_rejected(--rate nope)
expect_rejected(--sinks 999999999999999999999)
