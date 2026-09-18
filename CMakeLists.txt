cmake_minimum_required(VERSION 3.16)

project(mihoyo_launcher_background_downloader
        VERSION 1.0.0
        DESCRIPTION "Extract and download miHoYo launcher background resources from its Chromium cache"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The version reaches the code through a generated header rather than a
# -DDMBG_VERSION="..." flag: quoting a macro value on a command line is not
# portable (PowerShell in particular strips the inner quotes and the macro
# silently degrades into a number).
set(DMBG_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DMBG_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${DMBG_GENERATED_DIR}/version.h"
    @ONLY
)

add_executable(dmbg
    src/main.cpp
    src/url_scanner.cpp
    src/http_client.cpp
)

target_include_directories(dmbg PRIVATE src "${DMBG_GENERATED_DIR}")

# user32 for SetProcessDpiAwarenessContext, winhttp for HTTPS, shell32 for
# ShellExecute, ole32 for COM/IFileDialog, comdlg32 for the dialog base types.
target_link_libraries(dmbg PRIVATE winhttp shell32 ole32 comdlg32 user32)

if(MSVC)
    # Source files are UTF-8 (they contain Chinese literals); /utf-8 stops MSVC from
    # reading them as the ANSI code page. NOMINMAX keeps windows.h from defining
    # min/max as macros, which would break std::min / std::max.
    target_compile_options(dmbg PRIVATE /utf-8 /W4 /permissive- /EHsc)
    target_compile_definitions(dmbg PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX)
else()
    target_compile_options(dmbg PRIVATE -Wall -Wextra)
    target_compile_definitions(dmbg PRIVATE UNICODE _UNICODE NOMINMAX)
endif()

set_target_properties(dmbg PROPERTIES
    OUTPUT_NAME "dmbg"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
