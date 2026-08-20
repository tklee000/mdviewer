cmake_minimum_required(VERSION 3.21)
project(MdViewerLinux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project must be configured on Linux (Ubuntu 22.04 is the baseline).")
endif()
if(NOT CEF_ROOT)
    message(FATAL_ERROR "Set CEF_ROOT to a Linux 64-bit CEF binary distribution.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
find_package(X11 REQUIRED)

if(NOT EXISTS "${CEF_LIB_DEBUG}")
    set(CEF_LIB_DEBUG "${CEF_LIB_RELEASE}")
endif()
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

set(MDVIEWER_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
add_executable(mdviewer
    src/main.cpp
    "${MDVIEWER_ROOT}/platform/common/PortableCore.cpp"
    "${MDVIEWER_ROOT}/platform/common/PortableCef.cpp"
)
SET_EXECUTABLE_TARGET_PROPERTIES(mdviewer)
target_include_directories(mdviewer PRIVATE
    "${MDVIEWER_ROOT}/platform/common"
    "${CEF_ROOT}"
    ${X11_INCLUDE_DIR}
)
target_compile_definitions(mdviewer PRIVATE
    MDVIEWER_SOURCE_WEB_DIR="${MDVIEWER_ROOT}/web"
)
target_compile_options(mdviewer PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mdviewer PRIVATE
    libcef_lib
    libcef_dll_wrapper
    PkgConfig::GTK3
    ${X11_LIBRARIES}
)
add_dependencies(mdviewer libcef_dll_wrapper)
set_target_properties(mdviewer PROPERTIES
    OUTPUT_NAME "mdviewer"
    INSTALL_RPATH "$ORIGIN"
    BUILD_RPATH "$ORIGIN"
)

install(TARGETS mdviewer RUNTIME DESTINATION bin)
install(DIRECTORY "${CEF_BINARY_DIR}/" DESTINATION bin USE_SOURCE_PERMISSIONS)
install(DIRECTORY "${CEF_RESOURCE_DIR}/" DESTINATION bin)
install(DIRECTORY "${MDVIEWER_ROOT}/web/" DESTINATION share/mdviewer/web)
install(FILES "${MDVIEWER_ROOT}/assets/MdViewer.png"
    DESTINATION share/icons/hicolor/256x256/apps RENAME mdviewer.png)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/packaging/mdviewer.desktop"
    DESTINATION share/applications)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/packaging/io.mdviewer.MdViewer.metainfo.xml"
    DESTINATION share/metainfo)
if(EXISTS "${CEF_ROOT}/LICENSE.txt")
    install(FILES "${CEF_ROOT}/LICENSE.txt" DESTINATION share/licenses/mdviewer RENAME CEF-LICENSE.txt)
endif()
if(EXISTS "${CEF_ROOT}/CREDITS.html")
    install(FILES "${CEF_ROOT}/CREDITS.html" DESTINATION share/licenses/mdviewer RENAME CEF-CREDITS.html)
endif()
