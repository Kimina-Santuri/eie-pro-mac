cmake_minimum_required(VERSION 3.12.0)
project(EIEPro C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)

set(DRIVER_VERSION "0.1.0" CACHE STRING "Driver version")
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
# Static libusb: by default Homebrew's (this Mac's architecture, newest macOS only). For a
# release, package/build-libusb.sh builds a universal macOS 12+ copy and passes
# -DLIBUSB_DIR=build/libusb -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64".
set(LIBUSB_DIR "/opt/homebrew" CACHE PATH "Prefix holding include/libusb-1.0 and lib/libusb-1.0.a")
string(REPLACE ";" "|" ARCHS_ALT "${CMAKE_OSX_ARCHITECTURES}")

# libASPL, built and installed into the build tree
include(ExternalProject)
ExternalProject_Add(libASPL_ext
  SOURCE_DIR ${ROOT}/third_party/libASPL
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/libASPL-build
  INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix
  LIST_SEPARATOR |
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_OSX_ARCHITECTURES=${ARCHS_ALT} -DLIBASPL_VERSION=3.1.2
  BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix/lib/libASPL.a
  LOG_CONFIGURE YES LOG_BUILD YES LOG_INSTALL YES)

add_library(EIEPro MODULE EieDriver.cpp ${ROOT}/src/eie_usb.c)
add_dependencies(EIEPro libASPL_ext)
target_include_directories(EIEPro PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix/include ${ROOT}/src ${LIBUSB_DIR}/include)
target_compile_options(EIEPro PRIVATE -O2 -Wall)

# libusb linked statically, so the plug-in carries no outside dylib into the audio server
target_link_libraries(EIEPro PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix/lib/libASPL.a
  ${LIBUSB_DIR}/lib/libusb-1.0.a
  "-framework CoreFoundation" "-framework CoreAudio" "-framework IOKit" "-framework Security")

set_target_properties(EIEPro PROPERTIES
  OUTPUT_NAME "EIEPro"
  BUNDLE true
  BUNDLE_EXTENSION "driver"
  PREFIX "" SUFFIX ""
  MACOSX_BUNDLE ON
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_BUNDLE_NAME "EIE Pro"
  MACOSX_BUNDLE_BUNDLE_VERSION "${DRIVER_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${DRIVER_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2026 Kimina-Santuri. MIT licence."
  MACOSX_BUNDLE_GUI_IDENTIFIER "co.ke.kimina.eie-pro.driver")

# ad-hoc signature (no Apple developer certificate needed on your own Mac)
add_custom_command(TARGET EIEPro POST_BUILD
  COMMAND codesign --force --sign - "${CMAKE_CURRENT_BINARY_DIR}/EIEPro.driver"
  VERBATIM)
