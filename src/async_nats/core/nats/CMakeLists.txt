# Async NATS runtime

add_library(nats_async STATIC
    event_loop.cpp
    NatsAsioEventLoop.cpp
    include/core/nats/event_loop.hpp
    include/core/nats/NatsAsioEventLoop.hpp
)
add_library(Nats::Async ALIAS nats_async)

set_target_properties(nats_async PROPERTIES
    EXPORT_NAME Async
)

target_include_directories(nats_async PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(nats_async PUBLIC
    Nats::Client::Settings
    boost::boost
    cnats::nats_static
    spdlog::spdlog
)
