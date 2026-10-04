add_library(Error INTERFACE)
add_library(Core::Error ALIAS Error)

set_target_properties(Error PROPERTIES
    EXPORT_NAME Error
)

target_include_directories(Error INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
