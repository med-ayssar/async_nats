cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(async_nats VERSION 0.1.0 LANGUAGES CXX)
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

add_library(async_nats_objects OBJECT
    src/async_nats/client.cpp
    src/async_nats/jetstream.cpp
    src/async_nats/core.cpp
)
target_compile_features(async_nats_objects PUBLIC cxx_std_23)
target_include_directories(async_nats_objects PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/include>
)
target_link_libraries(async_nats_objects PUBLIC
    Boost::cobalt
    spdlog::spdlog
    Threads::Threads
)

add_library(async_nats STATIC
    src/async_nats/main.cpp
    src/async_nats/event_loop.cpp
    $<TARGET_OBJECTS:async_nats_objects>
)
add_library(Nats::async_nats ALIAS async_nats)
add_dependencies(async_nats async_nats_objects)

set_target_properties(async_nats PROPERTIES
    EXPORT_NAME async_nats
    OUTPUT_NAME async_nats
)

target_compile_features(async_nats PUBLIC cxx_std_23)

target_include_directories(async_nats PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(async_nats PUBLIC
    Boost::cobalt
    spdlog::spdlog
    Threads::Threads
)

install(TARGETS async_nats
    EXPORT async_natsTargets
    COMPONENT library
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES src/async_nats/include/async_nats.h
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES src/async_nats/include/async_nats/core.h
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/async_nats
)

install(EXPORT async_natsTargets
    FILE async_natsTargets.cmake
    NAMESPACE Nats::
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/async_nats
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/async_natsConfigVersion.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/async_natsConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/async_natsConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/async_nats
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/async_natsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/async_natsConfigVersion.cmake"
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/async_nats
)

option(BUILD_TESTS "Build the test targets" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(async_nats_tests tests/core_test.cpp)
    target_link_libraries(async_nats_tests PRIVATE async_nats_objects Catch2::Catch2WithMain)
    target_include_directories(async_nats_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/async_nats)
    add_test(NAME async_nats_tests COMMAND async_nats_tests)

    add_executable(pub_sub examples/pub_sub.cpp)
    target_link_libraries(pub_sub PRIVATE async_nats)
endif()

