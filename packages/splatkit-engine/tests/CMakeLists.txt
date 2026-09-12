add_executable(splatkit_engine_tests
  camera/WalkCameraTest.cpp
  rendering/GpuLayoutTest.cpp
  rendering/FrameOrderTest.cpp
)
target_link_libraries(splatkit_engine_tests PRIVATE splatkit_engine spz GTest::gtest_main)
target_compile_options(splatkit_engine_tests PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
)
include(GoogleTest)
gtest_discover_tests(splatkit_engine_tests)
