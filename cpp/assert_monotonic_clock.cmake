# Guards the P0 fix: MonotonicMs must never again be fed from the wall clock.
#
# Store::apply keeps `latest_` and Store::snapshot throws when handed an earlier
# instant, so one NTP correction backward would terminate the process mid-match.
# A forward jump is equally harmful: it lets the mode exit window "complete"
# across a sleep the operator never observed as clear samples.
#
# QDateTime::currentDateTimeUtc() in logging.cpp is deliberately NOT matched:
# that produces an ISO8601 log timestamp for humans, not a MonotonicMs.
file(GLOB sources "${SOURCE_DIR}/cpp/*.cpp" "${SOURCE_DIR}/cpp/*.h")
set(offenders "")
foreach(source IN LISTS sources)
    file(STRINGS "${source}" hits REGEX "current(M)?SecsSinceEpoch")
    if(hits)
        get_filename_component(name "${source}" NAME)
        foreach(hit IN LISTS hits)
            string(STRIP "${hit}" hit)
            # Prose naming the banned call is not a use of it. Comment-only lines
            # are skipped so the guard flags code; a trailing comment on a real
            # statement still starts with the statement and is still caught.
            if(NOT hit MATCHES "^(//|\\*|#)")
                list(APPEND offenders "${name}: ${hit}")
            endif()
        endforeach()
    endif()
endforeach()

if(offenders)
    string(REPLACE ";" "\n  " rendered "${offenders}")
    message(FATAL_ERROR
        "wall-clock time is being used as a MonotonicMs source:\n  ${rendered}\n"
        "Use rm_terminal::monotonic_now() from cpp/clock.h instead.")
endif()
message(STATUS "monotonic clock guard: no wall-clock MonotonicMs sources")
