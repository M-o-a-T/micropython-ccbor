# ccbor for ports built with CMake (esp32, rp2).
add_library(usermod_ccbor INTERFACE)

target_sources(usermod_ccbor INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modccbor.c
)

target_include_directories(usermod_ccbor INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_ccbor)
