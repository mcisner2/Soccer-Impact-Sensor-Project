#CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(soccer_backend CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# On Windows, make GoogleTest use the same C runtime as our code
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Everything except tests lives in this library so the tests (and later
# the real app) can both link against it.
add_library(backend_core
  src/packet_parser.cpp
  src/sequence_tracker.cpp
  src/models.cpp
  src/session_store.cpp
)
target_include_directories(backend_core PUBLIC src)

enable_testing()
add_executable(packet_parser_test tests/packet_parser_test.cpp)
target_link_libraries(packet_parser_test backend_core GTest::gtest_main)
add_test(NAME packet_parser_test COMMAND packet_parser_test)

add_executable(sequence_tracker_test tests/sequence_tracker_test.cpp)
target_link_libraries(sequence_tracker_test backend_core GTest::gtest_main)
add_test(NAME sequence_tracker_test COMMAND sequence_tracker_test)

add_executable(session_test tests/models_test.cpp tests/session_store_test.cpp)
target_link_libraries(session_test backend_core GTest::gtest_main)
add_test(NAME session_test COMMAND session_test)