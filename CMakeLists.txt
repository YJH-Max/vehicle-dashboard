cmake_minimum_required(VERSION 3.10)
project(VehicleDashboard)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TTYPE)
    set(CMAKE_BUILD_TTYPE Release)
endif()

include_directories(include)

add_executable(dashboard src/main.cpp src/data_generator.cpp)
target_link_libraries(dashboard pthread)

add_executable(test_ring tests/test_ring.cpp)
target_link_libraries(test_ring pthread)

enable_testing()
add_test(NAME ring_spsc_test COMMAND test_ring)
