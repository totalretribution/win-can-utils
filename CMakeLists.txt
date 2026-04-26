cmake_minimum_required(VERSION 3.14)
project(wincan VERSION 1.0.0 LANGUAGES C)

# -------------------------------------------------------------------------
# Library
# -------------------------------------------------------------------------
add_library(wincanlib STATIC src/wincanlib.c)

target_include_directories(wincanlib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(wincanlib PUBLIC winusb setupapi)
target_compile_options(wincanlib PRIVATE -Wall -Wextra)

# Alias so consumers always use wincan::wincan regardless of how they got it
add_library(wincan::wincan ALIAS wincanlib)

# -------------------------------------------------------------------------
# Optional components
# -------------------------------------------------------------------------
option(WINCAN_BUILD_TOOLS    "Build CLI utilities (candump, cangen, cansend, wincan)" OFF)
option(WINCAN_BUILD_EXAMPLES "Build examples" OFF)

# Version string passed to tool executables (e.g. -DWINCAN_VERSION=v1.2.3)
if(NOT DEFINED WINCAN_VERSION)
    set(WINCAN_VERSION "dev")
endif()

if(WINCAN_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(WINCAN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# -------------------------------------------------------------------------
# Install rules (for find_package after install)
# -------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS wincanlib
    EXPORT wincanTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT wincanTargets
    FILE wincanTargets.cmake
    NAMESPACE wincan::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wincan
)

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/wincanConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wincanConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/wincanConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wincan
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/wincanConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/wincanConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wincan
)
