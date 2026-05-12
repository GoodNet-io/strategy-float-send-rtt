cmake_minimum_required(VERSION 3.22)

# FloatSendRtt strategy plugin — Apache 2.0.
#
# OBJECT library carries the C++ class for in-tree tests; the
# `goodnet_strategy_float_send_rtt.so` plugin entry registers the
# `gn.strategy.rtt-optimal` extension through the host_api.

if(NOT TARGET GoodNet::sdk)
    project(goodnet_strategy_float_send_rtt
        VERSION   0.1.0
        LANGUAGES CXX)
    find_package(GoodNet REQUIRED)
    include(CTest)
    if(BUILD_TESTING)
        find_package(GTest      REQUIRED)
        find_package(rapidcheck QUIET)
        goodnet_patch_rapidcheck_targets()
        set(GOODNET_BUILD_TESTS ON)
    endif()
endif()

add_library(goodnet_float_send_rtt_objects OBJECT
    float_send_rtt.cpp
)
target_link_libraries(goodnet_float_send_rtt_objects PUBLIC GoodNet::sdk)
target_compile_features(goodnet_float_send_rtt_objects PUBLIC cxx_std_23)
target_compile_options(goodnet_float_send_rtt_objects PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter)
target_include_directories(goodnet_float_send_rtt_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(goodnet_float_send_rtt_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_plugin(goodnet_strategy_float_send_rtt
    plugin_entry.cpp
)
target_link_libraries(goodnet_strategy_float_send_rtt
    PRIVATE goodnet_float_send_rtt_objects)

if(GOODNET_BUILD_TESTS)
    add_subdirectory(tests)
endif()
