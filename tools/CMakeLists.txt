set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

foreach(tool candump cangen cansend)
    add_executable(${tool} ${tool}.c common.c)
    target_include_directories(${tool} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${tool} PRIVATE wincan::wincan)
    target_compile_definitions(${tool} PRIVATE VERSION="${WINCAN_VERSION}")
endforeach()
