cmake_minimum_required(VERSION 3.20)
project(hb_viewer VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# HBV_CLI_ONLY: build just the decoder + --convert tool (needs only Qt6 Gui), for testing.
option(HBV_CLI_ONLY "Build only the command-line converter" OFF)

set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_library(hbcore STATIC src/image.cpp src/psd.cpp src/decode.cpp src/stb_impl.cpp src/cli.cpp)
target_include_directories(hbcore PUBLIC src third_party)
target_link_libraries(hbcore PUBLIC Qt6::Core Qt6::Gui)
find_package(Threads REQUIRED)
target_link_libraries(hbcore PUBLIC Threads::Threads)
target_compile_options(hbcore PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

if(HBV_CLI_ONLY)
  add_executable(hb_convert src/cli_main.cpp)
  target_link_libraries(hb_convert hbcore)
  # offscreen render test of the image view (Qt Widgets only)
  find_package(Qt6 COMPONENTS Widgets)
  if(Qt6Widgets_FOUND)
    add_executable(hb_viewtest tests/viewtest.cpp src/imageview.cpp)
    target_link_libraries(hb_viewtest hbcore Qt6::Widgets)
  endif()
  return()
endif()

find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(KF6 REQUIRED COMPONENTS CoreAddons Parts KIO XmlGui ColorScheme ConfigWidgets)

add_executable(hb_viewer src/main.cpp src/mainwindow.cpp src/imageview.cpp src/columnbrowser.cpp src/previewpane.cpp)
target_link_libraries(hb_viewer hbcore Qt6::Widgets
  KF6::CoreAddons KF6::Parts KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets
  KF6::XmlGui KF6::ColorScheme KF6::ConfigWidgets)
# Embedded 3D viewer: libf3d (prebuilt SDK from https://github.com/f3d-app/f3d/releases,
# pass -DCMAKE_PREFIX_PATH=<sdk>). Without it, 3D files open in an external f3d.
find_package(f3d CONFIG QUIET COMPONENTS library)
if(f3d_FOUND)
  find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets)
  target_sources(hb_viewer PRIVATE src/modelview.cpp)
  target_link_libraries(hb_viewer f3d::libf3d Qt6::OpenGLWidgets)
  target_compile_definitions(hb_viewer PRIVATE HBV_HAVE_F3D)
  # AppImage layout: usr/bin/hb_viewer + usr/lib/libf3d*.so
  set_target_properties(hb_viewer PROPERTIES BUILD_RPATH "$ORIGIN/../lib;${f3d_DIR}/../../../lib" INSTALL_RPATH "$ORIGIN/../lib")
  message(STATUS "f3d found: ${f3d_VERSION} -> embedded 3D viewer enabled")
else()
  message(STATUS "f3d not found -> 3D files open in an external f3d")
endif()
target_compile_options(hb_viewer PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
install(TARGETS hb_viewer RUNTIME DESTINATION bin)
