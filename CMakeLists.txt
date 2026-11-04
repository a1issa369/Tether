cmake_minimum_required(VERSION 3.16)
project(tether VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TETHER_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TETHER_WERROR "Treat warnings as errors" ON)

add_library(tether STATIC
  src/crc32.cpp
  src/packet.cpp
  src/connection.cpp
  src/udp.cpp
  src/endpoint.cpp)
target_include_directories(tether PUBLIC include)
target_compile_options(tether PRIVATE -Wall -Wextra -Wpedantic)
if(TETHER_WERROR)
  target_compile_options(tether PRIVATE -Werror)
endif()
if(TETHER_SANITIZE)
  target_compile_options(tether PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(tether PUBLIC -fsanitize=address,undefined)
endif()

add_executable(tether_cli examples/tether_cli.cpp)
target_link_libraries(tether_cli PRIVATE tether)

enable_testing()
add_executable(tether_tests
  tests/test_main.cpp
  tests/test_packet.cpp
  tests/test_connection.cpp
  tests/test_udp.cpp)
target_include_directories(tether_tests PRIVATE tests)
target_link_libraries(tether_tests PRIVATE tether)
target_compile_options(tether_tests PRIVATE -Wall -Wextra -Wpedantic)
if(TETHER_WERROR)
  target_compile_options(tether_tests PRIVATE -Werror)
endif()
add_test(NAME tether_tests COMMAND tether_tests)
