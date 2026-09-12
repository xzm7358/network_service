if(NOT DEFINED ROOT)
    message(FATAL_ERROR "PRODUCT_ROUTE_OWNERSHIP_GUARD_INVALID: ROOT is required")
endif()

set(CONFIGURATOR "${ROOT}/src/platform/network_configurator.cpp")
if(NOT EXISTS "${CONFIGURATOR}")
    message(FATAL_ERROR "PRODUCT_ROUTE_OWNERSHIP_GUARD_INVALID: missing network_configurator.cpp")
endif()

file(READ "${CONFIGURATOR}" CONTENT)

# Never restore the old interface-wide deletion primitive. Route ownership must
# always include the exact gateway/metric identity tracked by Service.
string(FIND "${CONTENT}" "route del default dev " BLIND_DELETE_POS)
if(NOT BLIND_DELETE_POS EQUAL -1)
    message(FATAL_ERROR
        "PRODUCT_ROUTE_OWNERSHIP_LEAK: blind 'route del default dev <iface>' is forbidden")
endif()

# Exact deletion must continue to carry gateway, interface and metric. This
# source-level guard complements the typed C++ port signature and protects the
# shell command itself from regressing to a broader deletion.
string(FIND "${CONTENT}" "route del default gw %s dev %s metric %d" EXACT_DELETE_POS)
if(EXACT_DELETE_POS EQUAL -1)
    message(FATAL_ERROR
        "PRODUCT_ROUTE_OWNERSHIP_LEAK: exact default-route deletion command is missing")
endif()

message(STATUS "NetworkService exact route ownership guard: PASS")
