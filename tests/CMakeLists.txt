# FloatSendRtt strategy plugin unit tests — Apache 2.0.
include(GoogleTest)

add_executable(test_float_send_rtt test_float_send_rtt.cpp)
target_link_libraries(test_float_send_rtt PRIVATE
    GoodNet::sdk
    goodnet_float_send_rtt_objects
    GTest::gtest
    GTest::gtest_main)
target_compile_features(test_float_send_rtt PRIVATE cxx_std_23)
if(COMMAND goodnet_apply_pch)
    goodnet_apply_pch(test_float_send_rtt)
endif()
gtest_discover_tests(test_float_send_rtt
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 60)
