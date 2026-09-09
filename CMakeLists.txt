cmake_minimum_required(VERSION 3.10)
project(VehicleDashboard)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(include
    $ENV{HOME}/uWebSockets/src
    $ENV{HOME}/uWebSockets/uSockets/src)

add_executable(dashboard src/main.cpp src/data_generator.cpp)
target_link_libraries(dashboard pthread uSockets ssl crypto z)

add_executable(test_ring tests/test_ring.cpp)
target_link_libraries(test_ring pthread)

enable_testing()
add_test(NAME ring_spsc_test COMMAND test_ring)
