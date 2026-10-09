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

# The Nix compiler searches libc++ and NIX_CFLAGS_COMPILE on its own.
# CMake records those directories as implicit and drops them from
# compile_commands.json. Homebrew clangd then cannot see <thread>, so std
# is unknown, and it cannot see Boost or spdlog. Keep those directories on
# the command that clangd reads.
function(async_nats_show_include dir)
    if(dir STREQUAL "" OR dir MATCHES "^\\$<")
        return()
    endif()
    string(REGEX REPLACE "/+$" "" dir "${dir}")
    list(REMOVE_ITEM CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "${dir}")
    set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}" PARENT_SCOPE)
endfunction()

if(DEFINED ENV{NIX_CFLAGS_COMPILE})
    separate_arguments(_nix_cflags NATIVE_COMMAND "$ENV{NIX_CFLAGS_COMPILE}")
    set(_nix_take_next FALSE)
    foreach(_flag IN LISTS _nix_cflags)
        if(_nix_take_next)
            async_nats_show_include("${_flag}")
            set(_nix_take_next FALSE)
        elseif(_flag STREQUAL "-isystem" OR _flag STREQUAL "-I" OR _flag STREQUAL "-idirafter")
            set(_nix_take_next TRUE)
        elseif(_flag MATCHES "^(-isystem|-I|-idirafter)(.+)")
            async_nats_show_include("${CMAKE_MATCH_2}")
        endif()
    endforeach()
    unset(_nix_cflags)
    unset(_nix_take_next)
    unset(_flag)
endif()

set(ASYNC_NATS_LIBCXX_INCLUDE "")
execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -E -x c++ - -v
    INPUT_FILE /dev/null
    ERROR_VARIABLE _async_nats_cxx_verbose
    OUTPUT_QUIET
)
string(REGEX MATCHALL "/[^ \"\t\r\n]+/include/c\\+\\+/v1" _async_nats_libcxx_candidates "${_async_nats_cxx_verbose}")
foreach(_async_nats_libcxx IN LISTS _async_nats_libcxx_candidates)
    if(IS_DIRECTORY "${_async_nats_libcxx}")
        set(ASYNC_NATS_LIBCXX_INCLUDE "${_async_nats_libcxx}")
        break()
    endif()
endforeach()
if(ASYNC_NATS_LIBCXX_INCLUDE)
    async_nats_show_include("${ASYNC_NATS_LIBCXX_INCLUDE}")
endif()
unset(_async_nats_cxx_verbose)
unset(_async_nats_libcxx_candidates)
unset(_async_nats_libcxx)

find_package(Boost 1.90 REQUIRED CONFIG COMPONENTS cobalt)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

foreach(_async_nats_dep IN ITEMS Boost::headers Boost::cobalt spdlog::spdlog fmt::fmt)
    if(TARGET "${_async_nats_dep}")
        get_target_property(_async_nats_dep_includes "${_async_nats_dep}" INTERFACE_INCLUDE_DIRECTORIES)
        if(_async_nats_dep_includes)
            foreach(_async_nats_dep_include IN LISTS _async_nats_dep_includes)
                async_nats_show_include("${_async_nats_dep_include}")
            endforeach()
        endif()
    endif()
endforeach()
unset(_async_nats_dep)
unset(_async_nats_dep_includes)
unset(_async_nats_dep_include)

# Sources are listed explicitly. The event loop is
# src/AsyncNats/core/eventLoop/EventLoop.cpp.
set(ASYNC_NATS_CLIENT_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncNats/client/include)
set(ASYNC_NATS_CORE_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncNats/core/include)
set(ASYNC_NATS_EVENT_LOOP_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncNats/core/eventLoop/include)
set(ASYNC_NATS_UTILS_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncNats/core/utils/include)

add_library(AsyncNatsObjects OBJECT
    src/AsyncNats/client/Client.cpp
    src/AsyncNats/client/JetStream.cpp
    src/AsyncNats/core/Core.cpp
    src/AsyncNats/core/utils/ThreadCount.cpp
)
target_compile_features(AsyncNatsObjects PUBLIC cxx_std_23)
target_include_directories(AsyncNatsObjects PUBLIC
    $<BUILD_INTERFACE:${ASYNC_NATS_CLIENT_INCLUDE}>
    $<BUILD_INTERFACE:${ASYNC_NATS_CORE_INCLUDE}>
)
target_include_directories(AsyncNatsObjects PRIVATE
    ${ASYNC_NATS_UTILS_INCLUDE}
)
if(ASYNC_NATS_LIBCXX_INCLUDE)
    target_include_directories(AsyncNatsObjects SYSTEM PRIVATE "${ASYNC_NATS_LIBCXX_INCLUDE}")
endif()
target_link_libraries(AsyncNatsObjects PUBLIC
    Boost::cobalt
    spdlog::spdlog
    Threads::Threads
)

add_library(AsyncNats STATIC
    src/AsyncNats/core/Main.cpp
    src/AsyncNats/core/eventLoop/EventLoop.cpp
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
    ${ASYNC_NATS_EVENT_LOOP_INCLUDE}
)
if(ASYNC_NATS_LIBCXX_INCLUDE)
    target_include_directories(AsyncNats SYSTEM PRIVATE "${ASYNC_NATS_LIBCXX_INCLUDE}")
endif()

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

install(FILES src/AsyncNats/client/include/AsyncNats/client/AsyncNats.h
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/AsyncNats/client
)

install(FILES src/AsyncNats/core/include/AsyncNats/core/Core.h
    COMPONENT library
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/AsyncNats/core
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
    if(TARGET Catch2::Catch2)
        get_target_property(_async_nats_catch_includes Catch2::Catch2 INTERFACE_INCLUDE_DIRECTORIES)
        if(_async_nats_catch_includes)
            foreach(_async_nats_catch_include IN LISTS _async_nats_catch_includes)
                async_nats_show_include("${_async_nats_catch_include}")
            endforeach()
        endif()
        unset(_async_nats_catch_includes)
        unset(_async_nats_catch_include)
    endif()
    add_executable(AsyncNatsTests tests/core_test.cpp)
    target_link_libraries(AsyncNatsTests PRIVATE AsyncNatsObjects Catch2::Catch2WithMain)
    target_include_directories(AsyncNatsTests PRIVATE
        ${ASYNC_NATS_UTILS_INCLUDE}
    )
    if(ASYNC_NATS_LIBCXX_INCLUDE)
        target_include_directories(AsyncNatsTests SYSTEM PRIVATE "${ASYNC_NATS_LIBCXX_INCLUDE}")
    endif()
    add_test(NAME AsyncNatsTests COMMAND AsyncNatsTests)

    add_executable(pub_sub examples/pub_sub.cpp)
    target_link_libraries(pub_sub PRIVATE AsyncNats)
    if(ASYNC_NATS_LIBCXX_INCLUDE)
        target_include_directories(pub_sub SYSTEM PRIVATE "${ASYNC_NATS_LIBCXX_INCLUDE}")
    endif()
endif()
