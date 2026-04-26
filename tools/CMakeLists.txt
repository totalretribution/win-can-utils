set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

foreach(tool candump cangen cansend)
    add_executable(${tool} ${tool}.c)
    target_link_libraries(${tool} PRIVATE wincan::wincan)
    target_compile_definitions(${tool} PRIVATE VERSION="${WINCAN_VERSION}")
endforeach()

add_executable(canboth canboth.c)
target_link_libraries(canboth PRIVATE wincan::wincan)
target_compile_definitions(canboth PRIVATE VERSION="${WINCAN_VERSION}")
