cmake_minimum_required(VERSION 3.21)

project(MdViewer LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR
    "The common editor and browser boundaries are portable, but this first "
    "milestone currently provides only the Windows desktop shell.")
endif()

set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "Build configurations" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CEF_ROOT "" CACHE PATH "Path to an extracted CEF binary distribution")
if(NOT CEF_ROOT)
  message(FATAL_ERROR "Configure with -DCEF_ROOT=<extracted CEF directory>.")
endif()

set(CEF_RUNTIME_LIBRARY_FLAG "/MT" CACHE STRING "CEF MSVC runtime" FORCE)
set(USE_SANDBOX OFF CACHE BOOL "CEF sandbox library is not in the minimal distribution" FORCE)
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

if(NOT EXISTS "${CEF_LIB_DEBUG}")
  set(CEF_LIB_DEBUG "${CEF_LIB_RELEASE}")
endif()

add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

set(MDVIEWER_SOURCES
  src/App.cpp
  src/App.h
  src/BrowserHost.h
  src/CefBrowserHost.cpp
  src/CefRuntime.cpp
  src/CefRuntime.h
  src/Config.cpp
  src/Config.h
  src/DocxArchive.cpp
  src/DocxArchive.h
  src/FileAssociation.cpp
  src/FileAssociation.h
  src/GoogleDriveClient.cpp
  src/GoogleDriveClient.h
  src/HwpxArchive.cpp
  src/HwpxArchive.h
  src/Json.cpp
  src/Json.h
  src/Localization.cpp
  src/Localization.h
  src/MdzArchive.cpp
  src/MdzArchive.h
  src/PrinterService.cpp
  src/PrinterService.h
  src/libmzip/common.h
  src/libmzip/crc32.cpp
  src/libmzip/crc32.h
  src/libmzip/deflate_decoder.cpp
  src/libmzip/deflate_decoder.h
  src/libmzip/deflate_encoder.cpp
  src/libmzip/deflate_encoder.h
  src/libmzip/match.h
  src/libmzip/match_avx2.cpp
  src/libmzip/match_scalar.cpp
  src/libmzip/mzip_codec.cpp
  src/libmzip/platform.cpp
  src/libmzip/platform.h
  src/libmzip/thread_pool.cpp
  src/libmzip/thread_pool.h
  src/libmzip/mzip_codec.h
  src/libmzip/aes256_aesni.cpp
  src/libmzip/aes256_aesni.h
  src/libmzip/crypto.cpp
  src/libmzip/crypto.h
  src/libmzip/zip_archive.cpp
  src/libmzip/zip_archive.h
  src/RecentDocuments.cpp
  src/RecentDocuments.h
  src/UiResourceProvider.h
  src/UiResourceProviderWin.cpp
  src/app.rc
  src/main.cpp
  src/resource.h
)

set(MDVIEWER_UI_FILES
  web/index.html
  web/styles.css
  web/i18n.js
  web/app.js
  web/locales/de-DE.json
  web/locales/es-ES.json
  web/locales/fr-FR.json
  web/locales/hi-IN.json
  web/locales/id-ID.json
  web/locales/ja-JP.json
  web/locales/ko-KR.json
  web/locales/pt-BR.json
  web/locales/ru-RU.json
  web/locales/zh-CN.json
  web/locales/zh-TW.json
)
set_source_files_properties(${MDVIEWER_UI_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
set_property(SOURCE src/app.rc APPEND PROPERTY OBJECT_DEPENDS "${MDVIEWER_UI_FILES}")
set_property(SOURCE src/app.rc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/MdViewer.ico")

add_executable(MdViewer WIN32 ${MDVIEWER_SOURCES} ${MDVIEWER_UI_FILES})
target_include_directories(MdViewer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CEF_ROOT}")
target_compile_features(MdViewer PRIVATE cxx_std_20)
target_compile_definitions(MdViewer PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=NTDDI_WIN10_FE)
target_compile_options(MdViewer PRIVATE
  /MP /utf-8 /W4 /EHsc
  /wd4100 /wd4127 /wd4244 /wd4324 /wd4458 /wd4702 /wd4996)
target_link_options(MdViewer PRIVATE
  /MANIFEST:NO /LARGEADDRESSAWARE /STACK:0x800000)
target_link_libraries(MdViewer PRIVATE
  libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS}
  Advapi32.lib Bcrypt.lib Comdlg32.lib Crypt32.lib Dwmapi.lib Ole32.lib
  Gdi32.lib Shell32.lib WindowsApp.lib Windowscodecs.lib Winhttp.lib
  Winspool.lib Ws2_32.lib)
add_dependencies(MdViewer libcef_dll_wrapper)

if(NOT DEFINED MDVIEWER_OUTPUT_DIR)
  set(MDVIEWER_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/x64/$<CONFIG>")
endif()
set_target_properties(MdViewer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${MDVIEWER_OUTPUT_DIR}"
  VS_DEBUGGER_ENVIRONMENT
    "MDVIEWER_DEVELOPMENT_UI=${CMAKE_CURRENT_SOURCE_DIR}/web")

COPY_FILES(MdViewer "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR_RELEASE}" "${MDVIEWER_OUTPUT_DIR}")
set(MDVIEWER_CEF_CORE_RESOURCES
  chrome_100_percent.pak
  chrome_200_percent.pak
  icudtl.dat
  resources.pak)
COPY_FILES(MdViewer "${MDVIEWER_CEF_CORE_RESOURCES}" "${CEF_RESOURCE_DIR}" "${MDVIEWER_OUTPUT_DIR}")

set(MDVIEWER_CEF_LOCALES
  de.pak en-US.pak es.pak fr.pak hi.pak id.pak ja.pak ko.pak
  pt-BR.pak ru.pak zh-CN.pak zh-TW.pak)
add_custom_command(TARGET MdViewer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${MDVIEWER_OUTPUT_DIR}/locales")
foreach(locale_file IN LISTS MDVIEWER_CEF_LOCALES)
  COPY_SINGLE_FILE(MdViewer
    "${CEF_RESOURCE_DIR}/locales/${locale_file}"
    "${MDVIEWER_OUTPUT_DIR}/locales/${locale_file}")
endforeach()

add_custom_command(TARGET MdViewer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${MDVIEWER_OUTPUT_DIR}/licenses/cef"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CEF_ROOT}/LICENSE.txt" "${MDVIEWER_OUTPUT_DIR}/licenses/cef/LICENSE.txt"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CEF_ROOT}/CREDITS.html" "${MDVIEWER_OUTPUT_DIR}/licenses/cef/CREDITS.html"
  VERBATIM)

add_custom_command(TARGET MdViewer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${MDVIEWER_OUTPUT_DIR}/licenses/mdzip-spec"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/licenses/mdzip-spec/NOTICE.txt"
          "${MDVIEWER_OUTPUT_DIR}/licenses/mdzip-spec/NOTICE.txt"
  VERBATIM)

add_custom_command(TARGET MdViewer POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${MDVIEWER_OUTPUT_DIR}/licenses/owpml"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/licenses/owpml/NOTICE.txt"
          "${MDVIEWER_OUTPUT_DIR}/licenses/owpml/NOTICE.txt"
  VERBATIM)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES ${MDVIEWER_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/web" PREFIX "web" FILES ${MDVIEWER_UI_FILES})

PRINT_CEF_CONFIG()

include(CTest)
if(BUILD_TESTING)
  add_executable(MdViewerCoreTests
    tests/core-smoke.cpp
    src/DocxArchive.cpp
    src/DocxArchive.h
    src/GoogleDriveClient.cpp
    src/GoogleDriveClient.h
    src/HwpxArchive.cpp
    src/HwpxArchive.h
    src/Json.cpp
    src/Json.h
    src/MdzArchive.cpp
    src/MdzArchive.h
    src/libmzip/common.h
    src/libmzip/crc32.cpp
    src/libmzip/crc32.h
    src/libmzip/deflate_decoder.cpp
    src/libmzip/deflate_decoder.h
    src/libmzip/deflate_encoder.cpp
    src/libmzip/deflate_encoder.h
    src/libmzip/match.h
    src/libmzip/match_avx2.cpp
    src/libmzip/match_scalar.cpp
    src/libmzip/mzip_codec.cpp
    src/libmzip/platform.cpp
    src/libmzip/platform.h
    src/libmzip/thread_pool.cpp
    src/libmzip/thread_pool.h
    src/libmzip/mzip_codec.h
    src/RecentDocuments.cpp
    src/RecentDocuments.h)
  target_include_directories(MdViewerCoreTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(MdViewerCoreTests PRIVATE cxx_std_20)
  target_compile_definitions(MdViewerCoreTests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=NTDDI_WIN10_FE)
  target_compile_options(MdViewerCoreTests PRIVATE /utf-8 /W4 /EHsc)
  target_link_libraries(MdViewerCoreTests PRIVATE
    Advapi32.lib Bcrypt.lib Crypt32.lib Shell32.lib Winhttp.lib Ws2_32.lib)
  add_test(NAME MdViewerCoreTests COMMAND MdViewerCoreTests)
endif()
