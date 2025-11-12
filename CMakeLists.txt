cmake_minimum_required(VERSION 3.10)
project(roole C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -march=native")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include_directories(include)

# ------------------------------------------------------------------
# OPZIONI DI BUILD
# ------------------------------------------------------------------
option(BUILD_TRANSPORT           "Build solo roole_transport" ON)
option(BUILD_CORE                "Build roole_core" ON)
option(BUILD_CLUSTER             "Build roole_cluster" ON)
option(BUILD_GOSSIP              "Build roole_gossip" ON)
option(BUILD_RPC                 "Build roole_rpc" ON)
option(BUILD_DAG                 "Build roole_dag" ON)
option(BUILD_NODE                "Build roole_node" ON)
option(BUILD_EXECUTABLES         "Build eseguibili" ON)
option(BUILD_TESTS               "Build tutti i test" OFF)

# ------------------------------------------------------------------
# CORE LIBRARY
# ------------------------------------------------------------------
if(BUILD_CORE)
    add_library(roole_core STATIC
        src/core/common.c
        src/core/logger.c
        src/core/config.c
        src/core/config_validator.c
        src/core/metrics.c
        src/core/metrics_server.c
        src/core/event_bus.c
        src/core/service_registry.c
    )
    target_link_libraries(roole_core pthread m)
endif()

# ------------------------------------------------------------------
# TRANSPORT LIBRARY
# ------------------------------------------------------------------
if(BUILD_TRANSPORT)  # dipende da roole_core
    add_library(roole_transport STATIC
        src/transport/transport_base.c
        src/transport/udp_transport.c
        src/transport/tcp_transport.c
    )
    target_link_libraries(roole_transport roole_core)
endif()

# ------------------------------------------------------------------
# ALTRI LIBRARY (opzionali)
# ------------------------------------------------------------------
if(BUILD_CLUSTER)
    add_library(roole_cluster STATIC src/cluster/cluster_view.c src/cluster/membership.c)
    target_link_libraries(roole_cluster roole_core)
endif()

if(BUILD_GOSSIP)
    add_library(roole_gossip STATIC
        src/gossip/protocol/swim_state.c
        src/gossip/protocol/failure_detector.c
        src/gossip/protocol/message_handlers.c
        src/gossip/protocol/gossip_serialization.c
        src/gossip/engine/gossip_engine.c
    )
    target_link_libraries(roole_gossip roole_transport roole_cluster roole_core)
endif()

if(BUILD_RPC)
    add_library(roole_rpc STATIC
        src/rpc/core/rpc_channel.c
        src/rpc/core/rpc_serialization.c
        src/rpc/server/rpc_server.c
        src/rpc/client/rpc_client.c
    )
    target_link_libraries(roole_rpc roole_transport roole_core)
endif()

if(BUILD_DAG)
    add_library(roole_dag STATIC src/dag/dag_catalog.c src/dag/dag_executor.c)
    target_link_libraries(roole_dag roole_core)
endif()

if(BUILD_NODE)
    add_library(roole_node STATIC
        src/node/state/node_state.c
        src/node/peers/peer_pool.c
        src/node/handlers/handler_registry.c
    )
    target_link_libraries(roole_node PUBLIC roole_rpc roole_gossip roole_cluster roole_dag roole_core PRIVATE pthread m)
endif()

# ------------------------------------------------------------------
# ESEGUIBILI (opzionali)
# ------------------------------------------------------------------
if(BUILD_EXECUTABLES AND BUILD_NODE)
    add_executable(roole_node_bin src/node/node_main.c)
    target_link_libraries(roole_node_bin roole_node)
    set_target_properties(roole_node_bin PROPERTIES OUTPUT_NAME "roole-node")
endif()

# ------------------------------------------------------------------
# TEST
# ------------------------------------------------------------------
if(BUILD_TESTS AND TARGET roole_transport)
    enable_testing()

    add_executable(test_udp_transport test/unit/transport/test_udp_transport.c)
    target_link_libraries(test_udp_transport roole_transport)
    add_test(NAME test_udp_transport COMMAND test_udp_transport)

    add_executable(test_tcp_transport test/unit/transport/test_tcp_transport.c)
    target_link_libraries(test_tcp_transport roole_transport)
    add_test(NAME test_tcp_transport COMMAND test_tcp_transport)
endif()

if(BUILD_TESTS AND TARGET roole_gossip)
    enable_testing()

    add_executable(test_swim_state test/unit/gossip/test_swim_state.c)
    target_link_libraries(test_swim_state roole_gossip)
    add_test(NAME test_swim_state COMMAND test_swim_state)
endif()


# ------------------------------------------------------------------
# RIEPILOGO
# ------------------------------------------------------------------
message(STATUS "")
message(STATUS "🔧 ROOLE BUILD CONFIGURATION")
message(STATUS "  BUILD_TRANSPORT            = ${BUILD_TRANSPORT}")
message(STATUS "  BUILD_TESTS                = ${BUILD_TESTS}")
message(STATUS "  BUILD_CORE                 = ${BUILD_CORE}")
message(STATUS "  BUILD_CLUSTER              = ${BUILD_CLUSTER}")
message(STATUS "  BUILD_GOSSIP               = ${BUILD_GOSSIP}")
message(STATUS "  BUILD_RPC                  = ${BUILD_RPC}")
message(STATUS "  BUILD_DAG                  = ${BUILD_DAG}")
message(STATUS "  BUILD_NODE                 = ${BUILD_NODE}")
message(STATUS "  BUILD_EXECUTABLES          = ${BUILD_EXECUTABLES}")
message(STATUS "  BUILD_TESTS                = ${BUILD_TESTS}")
message(STATUS "")