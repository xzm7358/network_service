if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(source_path "${ROOT}/src/service/network_daemon.cpp")
file(READ "${source_path}" source)

set(stage_needle "if (!stage_ethernet_config(config_dir, desired, stage, error))")
set(apply_needle "if (!apply_ethernet_runtime(*control_plane, iface, desired, error))")
set(commit_needle "commit_ethernet_config(stage, error)")

string(FIND "${source}" "${stage_needle}" stage_pos)
string(FIND "${source}" "${apply_needle}" apply_pos)
string(FIND "${source}" "${commit_needle}" commit_pos)

if(stage_pos EQUAL -1 OR apply_pos EQUAL -1 OR commit_pos EQUAL -1)
    message(FATAL_ERROR "ETHERNET_TRANSACTION_GUARD: transaction stages are incomplete")
endif()
if(NOT stage_pos LESS apply_pos OR NOT apply_pos LESS commit_pos)
    message(FATAL_ERROR "ETHERNET_TRANSACTION_GUARD: required order is stage -> runtime apply -> commit")
endif()

string(FIND "${source}" "discard_ethernet_config(stage);" discard_pos)
if(discard_pos EQUAL -1)
    message(FATAL_ERROR "ETHERNET_TRANSACTION_GUARD: failed runtime path does not discard staged config")
endif()

string(REGEX MATCHALL
    "apply_ethernet_runtime\\(\\*control_plane, iface, previous, rollback_error\\)"
    rollback_calls "${source}")
list(LENGTH rollback_calls rollback_count)
if(rollback_count LESS 2)
    message(FATAL_ERROR
        "ETHERNET_TRANSACTION_GUARD: runtime and pre-commit failures must rollback previous config; count=${rollback_count}")
endif()

string(FIND "${source}" "EthernetConfigCommitResult::CommittedDurabilityUncertain" uncertain_pos)
string(FIND "${source}" "ETHERNET_CONFIG_DURABILITY_UNCERTAIN" uncertain_diag_pos)
if(uncertain_pos EQUAL -1 OR uncertain_diag_pos EQUAL -1)
    message(FATAL_ERROR "ETHERNET_TRANSACTION_GUARD: post-rename durability uncertainty is not explicit")
endif()

message(STATUS "ETHERNET_TRANSACTION_GUARD: PASS")
