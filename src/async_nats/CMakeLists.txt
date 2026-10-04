cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(async_nats VERSION 1.0.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release RelWithDebInfo "")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies.cmake")
endif()

set(ASYNC_NATS_VERSION 1.0.0)

add_library(async_nats STATIC main.cpp)
add_library(Nats::async_nats ALIAS async_nats)

set_target_properties(async_nats PROPERTIES
    EXPORT_NAME async_nats
    OUTPUT_NAME async_nats
)

target_compile_features(async_nats PUBLIC cxx_std_23)

target_include_directories(async_nats PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(async_nats PUBLIC
    boost::boost
    spdlog::spdlog
)

install(TARGETS async_nats
    EXPORT async_natsTargets
    COMPONENT library
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)

install(EXPORT async_natsTargets
    FILE async_natsTargets.cmake
    NAMESPACE Nats::
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/async_nats
)

set(CONFIG_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/async_nats)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/async_natsConfigVersion.cmake"
    VERSION ${ASYNC_NATS_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/async_natsConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/async_natsConfig.cmake"
    INSTALL_DESTINATION ${CONFIG_INSTALL_DIR}
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/async_natsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/async_natsConfigVersion.cmake"
    COMPONENT library
    DESTINATION ${CONFIG_INSTALL_DIR}
)
