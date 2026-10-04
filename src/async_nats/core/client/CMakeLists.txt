add_library(nats_client_settings
    client.cpp
    include/core/client/main.hpp
    include/core/client/Client.hpp
    include/core/client/settings.hpp
)
add_library(Nats::Client::Settings ALIAS nats_client_settings)
# target_link_options(nats_client_settings PRIVATE
#     "-Wl,--allow-shlib-undefined"
# )
target_link_options(nats_client_settings PRIVATE
    "-Wl,-undefined,dynamic_lookup"
)
# install(EXPORT) cannot emit a name with two '::' separators.
# The package config recreates Nats::Client::Settings from this export name.
set_target_properties(nats_client_settings PROPERTIES
    EXPORT_NAME ClientSettings
)

target_include_directories(nats_client_settings PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(nats_client_settings PUBLIC
    Core::Error
    boost::boost
    spdlog::spdlog
)
set_target_properties(nats_client_settings PROPERTIES
    INSTALL_NAME_DIR "@rpath"
)
