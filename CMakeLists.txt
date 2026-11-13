cmake_minimum_required(VERSION 3.20)
project(vnxfer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-deprecated-declarations)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Qt6 COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)

add_library(vnxfer_core
  src/core/socket.cpp
  src/core/des.cpp
  src/core/rfb_client.cpp
  src/core/ft/ultravnc.cpp
  src/core/ft/tightvnc2.cpp
  src/core/ft/tightvnc13.cpp
  src/core/ft/ultravnc_session.cpp
  src/core/ft/tightvnc2_session.cpp
  src/core/ft/tightvnc13_session.cpp
  src/core/ft/transfer.cpp
  src/core/ft/open.cpp
)
target_include_directories(vnxfer_core PUBLIC src)
target_link_libraries(vnxfer_core PUBLIC OpenSSL::Crypto ZLIB::ZLIB)

add_executable(vnxfer src/cli/main.cpp)
target_link_libraries(vnxfer PRIVATE vnxfer_core)

add_executable(vnxfer-gui
  src/gui/main.cpp
  src/gui/main_window.cpp
  src/gui/session_worker.cpp
)
target_link_libraries(vnxfer-gui PRIVATE vnxfer_core Qt6::Widgets)
