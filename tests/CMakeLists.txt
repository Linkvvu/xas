find_package(GTest CONFIG REQUIRED)

add_executable(xas_tests
    TestEcho.cpp
)

target_compile_definitions(xas_tests
  PRIVATE
    # ASIO_DISABLE_THREADS
    # ASIO_HANDLER_LOCATION
    # ASIO_ENABLE_HANDLER_TRACKING
)

target_link_libraries(xas_tests PRIVATE
    xas
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(xas_tests)
