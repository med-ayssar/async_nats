cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AsyncNats VERSION 0.1.0 LANGUAGES CXX)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release RelWithDebInfo "")

find_package(Boost 1.90 REQUIRED CONFIG COMPONENTS cobalt)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

# Listed explicitly. src/async_nats/core/client and core/error are the unused
# cnats tree and must stay out of the build. The event loop is
# src/async_nats/core/event_loop.cpp.
set(ASYNC_NATS_CLIENT_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/client/include)
set(ASYNC_NATS_CORE_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/core/include)

add_library(AsyncNatsObjects OBJECT
    src/async_nats/client/client.cpp
    src/async_nats/client/jetstream.cpp
    src/async_nats/core/core.cpp
    src/async_nats/utils/thread_count.cpp
)
target_compile_features(AsyncNatsObjects PUBLIC cxx_std_23)
target_include_directories(AsyncNatsObjects PUBLIC
    $<BUILD_INTERFACE:${ASYNC_NATS_CLIENT_INCLUDE}>
    $<BUILD_INTERFACE:${ASYNC_NATS_CORE_INCLUDE}>
)
target_include_directories(AsyncNatsObjects PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/client
    ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/utils
)
target_link_libraries(AsyncNatsObjects PUBLIC
    Boost::cobalt
    spdlog::spdlog
    Threads::Threads
)

add_library(AsyncNats STATIC
    src/async_nats/core/main.cpp
    src/async_nats/core/event_loop.cpp
    $<TARGET_OBJECTS:AsyncNatsObjects>
)
add_library(AsyncNats::AsyncNats ALIAS AsyncNats)
add_dependencies(AsyncNats AsyncNatsObjects)

set_target_properties(AsyncNats PROPERTIES
    EXPORT_NAME AsyncNats
    OUTPUT_NAME AsyncNats
)

target_compile_features(AsyncNats PUBLIC cxx_std_23)

target_include_directories(AsyncNats PUBLIC
    $<BUILD_INTERFACE:${ASYNC_NATS_CLIENT_INCLUDE}>
    $<BUILD_INTERFACE:${ASYNC_NATS_CORE_INCLUDE}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(AsyncNats PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/client
    ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/utils
)

target_link_libraries(AsyncNats PUBLIC
    Boost::cobalt
    spdlog::spdlog
    Threads::Threads
)

install(TARGETS AsyncNats
    EXPORT AsyncNatsTargets
    COMPONENT library
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES src/async_nats/client/include/AsyncNats.h
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES src/async_nats/core/include/AsyncNats/Core.h
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/AsyncNats
)

install(EXPORT AsyncNatsTargets
    FILE AsyncNatsTargets.cmake
    NAMESPACE AsyncNats::
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsyncNats
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AsyncNatsConfigVersion.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AsyncNatsConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AsyncNatsConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsyncNats
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AsyncNatsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AsyncNatsConfigVersion.cmake"
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AsyncNats
)

option(BUILD_TESTS "Build the test targets" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(AsyncNatsTests tests/core_test.cpp)
    target_link_libraries(AsyncNatsTests PRIVATE AsyncNatsObjects Catch2::Catch2WithMain)
    target_include_directories(AsyncNatsTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/utils
    )
    add_test(NAME AsyncNatsTests COMMAND AsyncNatsTests)

    add_executable(pub_sub examples/pub_sub.cpp)
    target_link_libraries(pub_sub PRIVATE AsyncNats)
endif()
