cmake_minimum_required(VERSION 3.10)

project(NetworkService VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NETWORK_SERVICE_TARGET network_service)

add_executable(${NETWORK_SERVICE_TARGET}
    src/main.cpp
    src/service/network_daemon.cpp
    src/service/network_state_change_detector.cpp
    src/ipc/network_ipc_server.cpp
    src/ipc/network_ipc_v1_business_dispatch.cpp
    src/ipc/network_ipc_v1_codec.cpp
    src/ipc/network_ipc_v1_event.cpp
    src/ipc/network_ipc_v1_outbound.cpp
    src/ipc/network_ipc_v1_rebase.cpp
    src/ipc/network_ipc_v1_session.cpp
    src/platform/interface_snapshot.cpp
    src/config/ethernet_config.cpp
    src/platform/ethernet_apply.cpp
    src/platform/wifi_backend.cpp
    src/platform/wpa_event_monitor.cpp
)

target_include_directories(${NETWORK_SERVICE_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(${NETWORK_SERVICE_TARGET} PRIVATE
    NETWORK_SERVICE_VERSION=\"${PROJECT_VERSION}\"
)

target_link_libraries(${NETWORK_SERVICE_TARGET} PRIVATE pthread)

set_target_properties(${NETWORK_SERVICE_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/app/smartcontrol/dnake/bin"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(network_ipc_v1_codec_test
        tests/network_ipc_v1_codec_test.cpp
        src/ipc/network_ipc_v1_codec.cpp
    )
    target_include_directories(network_ipc_v1_codec_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME network_ipc_v1_codec COMMAND network_ipc_v1_codec_test)

    add_executable(network_ipc_v1_event_test
        tests/network_ipc_v1_event_test.cpp
        src/ipc/network_ipc_v1_event.cpp
        src/ipc/network_ipc_v1_codec.cpp
    )
    target_include_directories(network_ipc_v1_event_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME network_ipc_v1_event COMMAND network_ipc_v1_event_test)

    add_executable(network_ipc_v1_outbound_test
        tests/network_ipc_v1_outbound_test.cpp
        src/ipc/network_ipc_v1_outbound.cpp
    )
    target_include_directories(network_ipc_v1_outbound_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME network_ipc_v1_outbound COMMAND network_ipc_v1_outbound_test)

    add_executable(network_ipc_v1_rebase_test
        tests/network_ipc_v1_rebase_test.cpp
        src/ipc/network_ipc_v1_rebase.cpp
        src/ipc/network_ipc_v1_codec.cpp
    )
    target_include_directories(network_ipc_v1_rebase_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME network_ipc_v1_rebase COMMAND network_ipc_v1_rebase_test)

    add_executable(network_ipc_v1_session_test
        tests/network_ipc_v1_session_test.cpp
        src/ipc/network_ipc_v1_session.cpp
        src/ipc/network_ipc_v1_codec.cpp
    )
    target_include_directories(network_ipc_v1_session_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME network_ipc_v1_session COMMAND network_ipc_v1_session_test)

    add_executable(network_state_change_detector_test
        tests/network_state_change_detector_test.cpp
        src/service/network_state_change_detector.cpp
    )
    target_include_directories(network_state_change_detector_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME network_state_change_detector COMMAND network_state_change_detector_test)

    add_executable(network_state_event_integration_test
        tests/network_state_event_integration_test.cpp
        src/service/network_daemon.cpp
        src/service/network_state_change_detector.cpp
        src/ipc/network_ipc_server.cpp
        src/ipc/network_ipc_v1_business_dispatch.cpp
        src/ipc/network_ipc_v1_codec.cpp
        src/ipc/network_ipc_v1_event.cpp
        src/ipc/network_ipc_v1_outbound.cpp
        src/ipc/network_ipc_v1_rebase.cpp
        src/ipc/network_ipc_v1_session.cpp
        src/platform/interface_snapshot.cpp
        src/config/ethernet_config.cpp
        src/platform/ethernet_apply.cpp
        src/platform/wifi_backend.cpp
        src/platform/wpa_event_monitor.cpp
    )
    target_include_directories(network_state_event_integration_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(network_state_event_integration_test PRIVATE
        NETWORK_SERVICE_VERSION=\"${PROJECT_VERSION}\"
    )
    target_link_libraries(network_state_event_integration_test PRIVATE pthread)
    add_test(NAME network_state_event_integration COMMAND network_state_event_integration_test)
endif()

install(TARGETS ${NETWORK_SERVICE_TARGET}
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
