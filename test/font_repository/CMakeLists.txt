add_executable(FontRepositoryTest
  FontRepositoryTest.cpp
  ${REPO_ROOT}/src/util/FontManifest.cpp
  ${REPO_ROOT}/src/util/FontRepositoryUtil.cpp
)

target_include_directories(FontRepositoryTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
)

target_link_libraries(FontRepositoryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontRepositoryTest)
