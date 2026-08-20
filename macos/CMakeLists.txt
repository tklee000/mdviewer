cmake_minimum_required(VERSION 3.21)
project(MdViewerMac LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version (must satisfy the selected CEF build)")

if(NOT APPLE)
    message(FATAL_ERROR "This project must be configured on macOS.")
endif()
if(NOT CEF_ROOT)
    message(FATAL_ERROR "Set CEF_ROOT to a macOS CEF binary distribution matching CMAKE_OSX_ARCHITECTURES.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
if(NOT EXISTS "${CEF_LIB_DEBUG}")
    set(CEF_LIB_DEBUG "${CEF_LIB_RELEASE}")
endif()
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

set(MDVIEWER_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(MDVIEWER_COMMON_SOURCES
    "${MDVIEWER_ROOT}/platform/common/PortableCore.cpp"
    "${MDVIEWER_ROOT}/platform/common/PortableCef.cpp"
)

add_executable(MdViewer MACOSX_BUNDLE src/main_mac.mm ${MDVIEWER_COMMON_SOURCES})
SET_EXECUTABLE_TARGET_PROPERTIES(MdViewer)
target_include_directories(MdViewer PRIVATE "${MDVIEWER_ROOT}/platform/common" "${CEF_ROOT}")
target_compile_definitions(MdViewer PRIVATE MDVIEWER_SOURCE_WEB_DIR="${MDVIEWER_ROOT}/web")
target_compile_options(MdViewer PRIVATE
    -Wall -Wextra -Wpedantic
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_link_libraries(MdViewer PRIVATE libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS} ${COCOA_FRAMEWORK})
add_dependencies(MdViewer libcef_dll_wrapper)
set_target_properties(MdViewer PROPERTIES
    OUTPUT_NAME "MdViewer"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/resources/Info.plist.in"
    INSTALL_RPATH "@executable_path/../Frameworks"
)

set(MDVIEWER_HELPER_TARGETS)
set(MDVIEWER_HELPER_INDEX 0)
foreach(HELPER_SUFFIX "" " (GPU)" " (Renderer)" " (Plugin)")
    math(EXPR MDVIEWER_HELPER_INDEX "${MDVIEWER_HELPER_INDEX} + 1")
    set(HELPER_TARGET "MdViewerHelper${MDVIEWER_HELPER_INDEX}")
    set(HELPER_NAME "MdViewer Helper${HELPER_SUFFIX}")
    add_executable(${HELPER_TARGET} MACOSX_BUNDLE src/helper_main.mm ${MDVIEWER_COMMON_SOURCES})
    SET_EXECUTABLE_TARGET_PROPERTIES(${HELPER_TARGET})
    target_include_directories(${HELPER_TARGET} PRIVATE "${MDVIEWER_ROOT}/platform/common" "${CEF_ROOT}")
    target_compile_options(${HELPER_TARGET} PRIVATE
        -Wall -Wextra -Wpedantic
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(${HELPER_TARGET} PRIVATE
        libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS} ${COCOA_FRAMEWORK})
    add_dependencies(${HELPER_TARGET} libcef_dll_wrapper)
    set_target_properties(${HELPER_TARGET} PROPERTIES
        OUTPUT_NAME "${HELPER_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/resources/Helper-Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${HELPER_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.mdviewer.MdViewer.helper${MDVIEWER_HELPER_INDEX}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.2.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "2"
        INSTALL_RPATH "@executable_path/../../../"
    )
    list(APPEND MDVIEWER_HELPER_TARGETS ${HELPER_TARGET})
endforeach()
add_dependencies(MdViewer ${MDVIEWER_HELPER_TARGETS})

set(MDVIEWER_FRAMEWORK_SOURCE
    "${CEF_ROOT}/Release/Chromium Embedded Framework.framework")
if(NOT EXISTS "${MDVIEWER_FRAMEWORK_SOURCE}")
    message(FATAL_ERROR "CEF framework was not found at ${MDVIEWER_FRAMEWORK_SOURCE}")
endif()

add_custom_command(TARGET MdViewer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Frameworks"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${MDVIEWER_FRAMEWORK_SOURCE}"
        "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Frameworks/Chromium Embedded Framework.framework"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${MDVIEWER_ROOT}/web" "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Resources/web"
    VERBATIM)

if(EXISTS "${CEF_ROOT}/LICENSE.txt")
    add_custom_command(TARGET MdViewer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Resources/licenses/cef"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CEF_ROOT}/LICENSE.txt"
            "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Resources/licenses/cef/LICENSE.txt"
        VERBATIM)
endif()
if(EXISTS "${CEF_ROOT}/CREDITS.html")
    add_custom_command(TARGET MdViewer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Resources/licenses/cef"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CEF_ROOT}/CREDITS.html"
            "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Resources/licenses/cef/CREDITS.html"
        VERBATIM)
endif()

set(MDVIEWER_HELPER_INDEX 0)
foreach(HELPER_SUFFIX "" " (GPU)" " (Renderer)" " (Plugin)")
    math(EXPR MDVIEWER_HELPER_INDEX "${MDVIEWER_HELPER_INDEX} + 1")
    set(HELPER_TARGET "MdViewerHelper${MDVIEWER_HELPER_INDEX}")
    set(HELPER_NAME "MdViewer Helper${HELPER_SUFFIX}")
    add_custom_command(TARGET MdViewer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_BUNDLE_DIR:${HELPER_TARGET}>"
            "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Frameworks/${HELPER_NAME}.app"
        VERBATIM)
endforeach()

set(MDVIEWER_ICON_FILE "" CACHE FILEPATH "Generated MdViewer.icns file")
if(MDVIEWER_ICON_FILE AND EXISTS "${MDVIEWER_ICON_FILE}")
    add_custom_command(TARGET MdViewer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MDVIEWER_ICON_FILE}" "$<TARGET_BUNDLE_DIR:MdViewer>/Contents/Resources/MdViewer.icns"
        VERBATIM)
else()
    message(WARNING "MDVIEWER_ICON_FILE is not set; build-dmg.sh generates the .icns file automatically.")
endif()
