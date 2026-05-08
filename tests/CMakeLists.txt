find_package(GTest CONFIG REQUIRED)

add_executable(xas_tests
    TestEcho.cpp
)
target_link_libraries(xas_tests PRIVATE
    xas
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(xas_tests)
