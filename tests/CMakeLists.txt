# Tests for the averaging core.
#
# Run them with:
#   ctest --test-dir build --output-on-failure
#
# There is no test framework on purpose: the program is small, and an extra
# dependency (GoogleTest/Catch2) would cost more than the tests are worth.
# test_average.cpp contains a few macros and a runner instead.
#
# average_data.cpp is compiled directly into the test executable (see the note
# about incremental linking in the top-level CMakeLists.txt).

add_executable(test_average test_average.cpp ${PROJECT_SOURCE_DIR}/average_data.cpp)
target_link_libraries(test_average PRIVATE ${NAME}_deps Threads::Threads)

add_test(NAME average_algorithm COMMAND test_average)
set_tests_properties(average_algorithm PROPERTIES TIMEOUT 300)

# On Windows the tests need the GDAL runtime (gdal.dll and its own dependencies).
# vcpkg copies it next to the executables, other installations (OSGeo4W, conda,
# a manual build) may not, so make sure the GDAL binary directory is on PATH.
if (WIN32 AND TARGET GDAL::GDAL AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
  set_tests_properties(average_algorithm PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:GDAL::GDAL>")
endif()
