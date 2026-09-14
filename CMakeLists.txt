cmake_minimum_required(VERSION 3.10)
project(VehicleDashboard)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(include
    /usr/local/include/uWebSockets
    $ENV{HOME}/uWebSockets/src
    $ENV{HOME}/uWebSockets/uSockets/src)

add_executable(dashboard src/main.cpp src/data_generator.cpp)
target_link_libraries(dashboard pthread uSockets ssl crypto z)

add_executable(test_ring tests/test_ring.cpp)
target_link_libraries(test_ring pthread)

enable_testing()
add_test(NAME ring_spsc_test COMMAND test_ring)

add_executable(can_producer src/can_producer.cpp)

add_executable(bench_mpmc tests/bench_mpmc.cpp)

add_executable(test_topic_bus tests/test_topic_bus.cpp)
target_link_libraries(test_topic_bus pthread)
add_test(NAME topic_bus_test COMMAND test_topic_bus)

add_executable(test_data_pool tests/test_data_pool.cpp)
target_link_libraries(test_data_pool pthread)
add_test(NAME data_pool_test COMMAND test_data_pool)

add_executable(bench_mutex_vs tests/bench_mutex_vs_lockfree.cpp)
target_link_libraries(bench_mutex_vs pthread)

add_executable(shm_writer src/shm_writer.cpp)
target_link_libraries(shm_writer pthread)

add_executable(shm_reader src/shm_reader.cpp)
target_link_libraries(shm_reader pthread)

add_executable(can_reader src/can_reader.cpp)
target_link_libraries(can_reader pthread)

add_executable(test_epoll_reporter tests/test_epoll_reporter.cpp)
target_link_libraries(test_epoll_reporter pthread)
