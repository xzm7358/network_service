if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(source_path "${ROOT}/src/platform/wifi_backend.cpp")
file(READ "${source_path}" source)

string(FIND "${source}" "auto rollback_new_profile" rollback_helper)
if(rollback_helper EQUAL -1)
    message(FATAL_ERROR "WIFI_PROFILE_ROLLBACK_GUARD: missing rollback helper after ADD_NETWORK")
endif()

string(FIND "${source}" "\"REMOVE_NETWORK \" + std::to_string(id)" remove_command)
if(remove_command EQUAL -1)
    message(FATAL_ERROR "WIFI_PROFILE_ROLLBACK_GUARD: transient profile is not removed")
endif()

string(FIND "${source}" "std::string ignored;" ignored_error)
if(ignored_error EQUAL -1)
    message(FATAL_ERROR "WIFI_PROFILE_ROLLBACK_GUARD: rollback may overwrite the original failure")
endif()

string(REGEX MATCHALL "return fail_after_add\\(\\);" rollback_returns "${source}")
list(LENGTH rollback_returns rollback_return_count)
if(rollback_return_count LESS 4)
    message(FATAL_ERROR
        "WIFI_PROFILE_ROLLBACK_GUARD: not all post-ADD_NETWORK failure paths rollback; count=${rollback_return_count}")
endif()

message(STATUS "WIFI_PROFILE_ROLLBACK_GUARD: PASS")
