cmake_minimum_required(VERSION 3.10)

project(NetworkService VERSION 0.1.0 LANGUAGES CXX)

option(NETWORK_SERVICE_BUILD_RC_PROBE
    "Build the target-side IPC v1 RC evidence probe"
    OFF)

find_package(Threads REQUIRED)

# Shared compile contract only. Internal source-tree headers are deliberately not
# exposed here: every runtime layer publishes only its own namespaced include root.
add_library(network_service_build_config INTERFACE)
target_compile_features(network_service_build_config INTERFACE cxx_std_17)
target_include_directories(network_service_build_config INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_definitions(network_service_build_config INTERFACE
    NETWORK_SERVICE_VERSION=\"${PROJECT_VERSION}\"
)

# Platform Mechanism: Linux/daemon/persistence mechanics only. Platform can see
# neutral contracts plus Platform/Config headers, but not Service or IPC headers.
add_library(network_service_platform STATIC
    src/platform/dhcp_lease_store.cpp
    src/platform/interface_snapshot.cpp
    src/platform/netlink_monitor.cpp
    src/platform/network_configurator.cpp
    src/config/ethernet_config.cpp
    src/platform/udhcpc_process.cpp
    src/platform/wifi_backend.cpp
    src/platform/wpa_ctrl_client.cpp
    src/platform/wpa_event_monitor.cpp
)
target_include_directories(network_service_platform
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/config/include>
)
target_link_libraries(network_service_platform
    PUBLIC
        network_service_build_config
        Threads::Threads
)
add_library(NetworkService::Platform ALIAS network_service_platform)

# Service / Policy owns cross-boundary contracts through include/. Platform is an
# implementation dependency only: its headers/libraries are available while
# compiling Service, but are not exported to Service consumers such as IPC.
add_library(network_service_service STATIC
    src/service/ethernet_manager.cpp
    src/service/ethernet_startup.cpp
    src/service/network_control_plane.cpp
    src/service/network_control_plane_reconcile.cpp
    src/service/network_daemon.cpp
    src/service/network_daemon_events.cpp
    src/service/network_state.cpp
    src/service/network_state_change_detector.cpp
    src/service/wifi_manager.cpp
    src/service/wifi_profile_policy.cpp
    src/service/wifi_scan_lifecycle.cpp
)
target_include_directories(network_service_service
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/service/include>
)
target_link_libraries(network_service_service
    PUBLIC
        network_service_build_config
    PRIVATE
        NetworkService::Platform
)
add_library(NetworkService::Service ALIAS network_service_service)

# IPC sees only public contracts plus Service/IPC headers. Platform/Config headers
# are no longer transitively visible through NetworkService::Service.
add_library(network_service_ipc_codec STATIC
    src/ipc/network_ipc_v1_codec.cpp
)
target_include_directories(network_service_ipc_codec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/include>
)
target_link_libraries(network_service_ipc_codec PUBLIC network_service_build_config)
add_library(NetworkService::IpcCodec ALIAS network_service_ipc_codec)

add_library(network_service_client STATIC
    src/client/network_service_client.cpp
)
target_include_directories(network_service_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/include
)
target_link_libraries(network_service_client PRIVATE NetworkService::IpcCodec)
add_library(NetworkService::Client ALIAS network_service_client)

add_library(network_service_ipc STATIC
    src/ipc/network_ipc_server.cpp
    src/ipc/network_ipc_representation.cpp
    src/ipc/network_ipc_v1_business_dispatch.cpp
    src/ipc/network_ipc_v1_event.cpp
    src/ipc/network_ipc_v1_outbound.cpp
    src/ipc/network_ipc_v1_rebase.cpp
    src/ipc/network_ipc_v1_session.cpp
)
target_include_directories(network_service_ipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/include>
)
target_link_libraries(network_service_ipc
    PUBLIC
        network_service_build_config
        NetworkService::IpcCodec
        NetworkService::Service
)
add_library(NetworkService::IPC ALIAS network_service_ipc)

set(NETWORK_SERVICE_TARGET network_service)
add_executable(${NETWORK_SERVICE_TARGET}
    src/main.cpp
)
target_link_libraries(${NETWORK_SERVICE_TARGET}
    PRIVATE
        NetworkService::IPC
)
set_target_properties(${NETWORK_SERVICE_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/app/smartcontrol/dnake/bin"
)

add_executable(networkctl
    tools/networkctl.cpp
)
target_link_libraries(networkctl
    PRIVATE
        NetworkService::Client
)
# tools/networkctl.cpp uses std::string_view; the superbuild baseline is C++14
# and NetworkService::Client does not propagate the C++17 requirement.
target_compile_features(networkctl PRIVATE cxx_std_17)
set_target_properties(networkctl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/app/smartcontrol/dnake/bin"
)

include(CTest)

# The RC probe is built automatically by host test configurations so its code
# stays under Werror/sanitizer pressure. Target superbuilds can request it with
# -DBUILD_TESTING=OFF -DNETWORK_SERVICE_BUILD_RC_PROBE=ON and deploy it beside
# network_service without enabling the rest of the test suite.
if(BUILD_TESTING OR NETWORK_SERVICE_BUILD_RC_PROBE)
    add_executable(network_service_rc_probe
        tools/rc/network_service_rc_probe.cpp
    )
    target_link_libraries(network_service_rc_probe PRIVATE NetworkService::IPC)
endif()

if(BUILD_TESTING)
    add_executable(network_service_client_test
        tests/network_service_client_test.cpp
    )
    target_link_libraries(network_service_client_test PRIVATE
        NetworkService::Client
        NetworkService::IpcCodec
    )
    add_test(NAME network_service_client COMMAND network_service_client_test)

    add_executable(networkctl_cli_test
        tests/networkctl_cli_test.cpp
    )
    target_link_libraries(networkctl_cli_test PRIVATE NetworkService::IpcCodec)
    add_test(NAME networkctl_cli
        COMMAND networkctl_cli_test $<TARGET_FILE:networkctl>)

    add_test(NAME route_ownership_guard
        COMMAND ${CMAKE_COMMAND}
            -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/route_ownership_guard.cmake)

    add_test(NAME wifi_platform_profile_rollback_guard
        COMMAND ${CMAKE_COMMAND}
            -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/wifi_platform_profile_rollback_guard.cmake)

    add_test(NAME ethernet_runtime_persistence_transaction_guard
        COMMAND ${CMAKE_COMMAND}
            -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ethernet_runtime_persistence_transaction_guard.cmake)

    add_test(NAME network_service_supervisor
        COMMAND sh
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/network_service_supervisor_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/packaging/S40network_service)

    add_test(NAME rcs_ethernet_migration
        COMMAND sh
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/rcs_ethernet_migration_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/packaging/migrate_rcs_ethernet.sh
            $<TARGET_FILE:network_service>)

    add_test(NAME ethernet_json_capability
        COMMAND $<TARGET_FILE:network_service>
            --check-capability ethernet-json-v1)

    add_test(NAME ethernet_config_validation
        COMMAND sh
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/ethernet_config_validation_test.sh
            $<TARGET_FILE:network_service>)

    add_executable(network_ipc_v1_codec_test
        tests/network_ipc_v1_codec_test.cpp
    )
    target_link_libraries(network_ipc_v1_codec_test PRIVATE NetworkService::IPC)
    add_test(NAME network_ipc_v1_codec COMMAND network_ipc_v1_codec_test)

    add_executable(network_ipc_v1_event_test
        tests/network_ipc_v1_event_test.cpp
    )
    target_link_libraries(network_ipc_v1_event_test PRIVATE NetworkService::IPC)
    add_test(NAME network_ipc_v1_event COMMAND network_ipc_v1_event_test)

    add_executable(network_ipc_v1_outbound_test
        tests/network_ipc_v1_outbound_test.cpp
    )
    target_link_libraries(network_ipc_v1_outbound_test PRIVATE NetworkService::IPC)
    add_test(NAME network_ipc_v1_outbound COMMAND network_ipc_v1_outbound_test)

    add_executable(network_ipc_v1_rebase_test
        tests/network_ipc_v1_rebase_test.cpp
    )
    target_link_libraries(network_ipc_v1_rebase_test PRIVATE NetworkService::IPC)
    add_test(NAME network_ipc_v1_rebase COMMAND network_ipc_v1_rebase_test)

    add_executable(network_ipc_v1_session_test
        tests/network_ipc_v1_session_test.cpp
    )
    target_link_libraries(network_ipc_v1_session_test PRIVATE NetworkService::IPC)
    add_test(NAME network_ipc_v1_session COMMAND network_ipc_v1_session_test)

    add_executable(network_ipc_representation_test
        tests/network_ipc_representation_test.cpp
    )
    target_link_libraries(network_ipc_representation_test PRIVATE NetworkService::IPC)
    add_test(NAME network_ipc_representation COMMAND network_ipc_representation_test)

    add_executable(wifi_scan_lifecycle_test
        tests/wifi_scan_lifecycle_test.cpp
    )
    target_link_libraries(wifi_scan_lifecycle_test PRIVATE NetworkService::Service)
    add_test(NAME wifi_scan_lifecycle COMMAND wifi_scan_lifecycle_test)

    add_executable(wifi_manager_test
        tests/wifi_manager_test.cpp
    )
    target_link_libraries(wifi_manager_test PRIVATE NetworkService::Service)
    add_test(NAME wifi_manager COMMAND wifi_manager_test)

    add_executable(wifi_profile_policy_test
        tests/wifi_profile_policy_test.cpp
    )
    target_link_libraries(wifi_profile_policy_test PRIVATE NetworkService::Service)
    add_test(NAME wifi_profile_policy COMMAND wifi_profile_policy_test)

    add_executable(network_control_plane_test
        tests/network_control_plane_test.cpp
    )
    target_link_libraries(network_control_plane_test PRIVATE NetworkService::Service)
    add_test(NAME network_control_plane COMMAND network_control_plane_test)

    add_executable(ethernet_startup_test
        tests/ethernet_startup_test.cpp
    )
    target_link_libraries(ethernet_startup_test PRIVATE NetworkService::Service)
    add_test(NAME ethernet_startup COMMAND ethernet_startup_test)

    add_executable(ethernet_manager_test
        tests/ethernet_manager_test.cpp
    )
    target_link_libraries(ethernet_manager_test PRIVATE NetworkService::Service)
    add_test(NAME ethernet_manager COMMAND ethernet_manager_test)

    add_executable(dns_brownfield_ownership_test
        tests/dns_brownfield_ownership_test.cpp
    )
    target_link_libraries(dns_brownfield_ownership_test PRIVATE NetworkService::Service)
    add_test(NAME dns_brownfield_ownership COMMAND dns_brownfield_ownership_test)

    add_executable(network_state_test
        tests/network_state_test.cpp
    )
    target_link_libraries(network_state_test PRIVATE NetworkService::Service)
    add_test(NAME network_state COMMAND network_state_test)

    add_executable(service_contract_visibility_test
        tests/service_contract_visibility_test.cpp
    )
    target_link_libraries(service_contract_visibility_test PRIVATE NetworkService::Service)
    add_test(NAME service_contract_visibility COMMAND service_contract_visibility_test)

    add_executable(ethernet_config_transaction_test
        tests/ethernet_config_transaction_test.cpp
    )
    target_link_libraries(ethernet_config_transaction_test PRIVATE NetworkService::Platform)
    add_test(NAME ethernet_config_transaction COMMAND ethernet_config_transaction_test)

    add_executable(dhcp_lease_store_test
        tests/dhcp_lease_store_test.cpp
    )
    target_link_libraries(dhcp_lease_store_test PRIVATE NetworkService::Platform)
    add_test(NAME dhcp_lease_store COMMAND dhcp_lease_store_test)

    add_executable(udhcpc_process_test
        tests/udhcpc_process_test.cpp
    )
    target_link_libraries(udhcpc_process_test PRIVATE NetworkService::Platform)
    add_test(NAME udhcpc_process COMMAND udhcpc_process_test)

    add_executable(netlink_monitor_test
        tests/netlink_monitor_test.cpp
    )
    target_link_libraries(netlink_monitor_test PRIVATE NetworkService::Platform)
    add_test(NAME netlink_monitor COMMAND netlink_monitor_test)

    add_executable(wpa_ctrl_client_test
        tests/wpa_ctrl_client_test.cpp
    )
    target_link_libraries(wpa_ctrl_client_test PRIVATE NetworkService::Platform)
    add_test(NAME wpa_ctrl_client COMMAND wpa_ctrl_client_test)

    add_executable(wpa_event_monitor_test
        tests/wpa_event_monitor_test.cpp
    )
    target_link_libraries(wpa_event_monitor_test PRIVATE NetworkService::Platform)
    add_test(NAME wpa_event_monitor COMMAND wpa_event_monitor_test)

    add_executable(network_state_change_detector_test
        tests/network_state_change_detector_test.cpp
    )
    target_link_libraries(network_state_change_detector_test PRIVATE NetworkService::Service)
    add_test(NAME network_state_change_detector COMMAND network_state_change_detector_test)

    add_executable(network_state_event_integration_test
        tests/network_state_event_integration_test.cpp
    )
    target_link_libraries(network_state_event_integration_test PRIVATE NetworkService::IPC)
    add_test(NAME network_state_event_integration COMMAND network_state_event_integration_test)

    # Standalone real-producer fixture for cross-repository SmartControl E2E.
    # It uses the production daemon/server implementation and only replaces the
    # existing SnapshotProvider seam with a deterministic file-driven source.
    add_executable(network_service_e2e_fixture
        tests/network_service_e2e_fixture_main.cpp
    )
    target_link_libraries(network_service_e2e_fixture PRIVATE NetworkService::IPC)
endif()

install(TARGETS ${NETWORK_SERVICE_TARGET}
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")

install(TARGETS networkctl
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")

install(PROGRAMS packaging/migrate_rcs_ethernet.sh
    DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
    RENAME network_service_migrate_rcs_ethernet)

if(NETWORK_SERVICE_BUILD_RC_PROBE)
    install(TARGETS network_service_rc_probe
        RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
    install(PROGRAMS tools/rc/ssd20x_collect.sh
        DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
endif()
