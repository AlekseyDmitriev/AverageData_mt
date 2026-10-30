cmake_minimum_required (VERSION 3.16...3.30)

# ---------------------------------------------------------------------------
# Where do the dependencies come from?
#
# GDAL and plog are looked up with plain CMake mechanisms, so the project builds
# with any installation: system packages (Linux/macOS), OSGeo4W, conda, a
# hand-built GDAL, conan, or vcpkg.
#
# vcpkg is used automatically only when it is actually available:
#   * explicitly:  cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   * implicitly:  VCPKG_ROOT environment variable is set
# There is no machine-specific path hard-coded anymore.
#
# Optional hints:
#   -DGDAL_ROOT=<prefix>      prefix that contains include/gdal.h and lib/gdal
#   -DGDAL_DIR=<dir>          directory that contains GDALConfig.cmake
#   -DPLOG_ROOT=<prefix>      prefix that contains include/plog/Log.h
#   -DCMAKE_PREFIX_PATH=<dirs> any prefixes to search first
# ---------------------------------------------------------------------------

if (NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(_vcpkg_toolchain "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  if (EXISTS "${_vcpkg_toolchain}")
    set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_toolchain}" CACHE FILEPATH "vcpkg toolchain file")
    message(STATUS "Using vcpkg toolchain from VCPKG_ROOT: ${CMAKE_TOOLCHAIN_FILE}")
  endif()
endif()

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

message("-- Setting C++20")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED on)

set(NAME AverageData_mt)

project(${NAME} LANGUAGES CXX)

find_package(Threads REQUIRED)

# Dependencies are collected in one interface target, so the executable always
# links the same thing regardless of how the libraries were found.
add_library(${NAME}_deps INTERFACE)

# --- GDAL ------------------------------------------------------------------
# 1) imported target: vcpkg, conan, GDAL >= 3.5 installed config
# 2) CMake's own FindGDAL module: OSGeo4W, conda, system packages
# 3) manual search using GDAL_ROOT / GDAL_DIR hints
find_package(GDAL CONFIG QUIET)

if (NOT TARGET GDAL::GDAL)
  find_package(GDAL QUIET) # module mode, FindGDAL.cmake ships with CMake
endif()

if (TARGET GDAL::GDAL)
  message(STATUS "GDAL: found as imported target GDAL::GDAL")
  target_link_libraries(${NAME}_deps INTERFACE GDAL::GDAL)
elseif (GDAL_FOUND)
  message(STATUS "GDAL: found by FindGDAL module (${GDAL_INCLUDE_DIRS})")
  target_include_directories(${NAME}_deps INTERFACE ${GDAL_INCLUDE_DIRS} ${GDAL_INCLUDE_DIR})
  target_link_libraries(${NAME}_deps INTERFACE ${GDAL_LIBRARIES} ${GDAL_LIBRARY})
else ()
  set(_gdal_hints ${GDAL_ROOT} $ENV{GDAL_ROOT} $ENV{OSGEO4W_ROOT})
  find_path(GDAL_INCLUDE_DIR gdal.h
    HINTS ${_gdal_hints} PATH_SUFFIXES include include/gdal)
  find_library(GDAL_LIBRARY NAMES gdal gdal_i
    HINTS ${_gdal_hints} PATH_SUFFIXES lib lib64 lib/x64)
  if (NOT GDAL_INCLUDE_DIR OR NOT GDAL_LIBRARY)
    message(FATAL_ERROR
      "GDAL was not found.\n"
      "Install it (OSGeo4W, conda, vcpkg, apt/brew ...) or point CMake at it:\n"
      "  -DGDAL_ROOT=<prefix with include/gdal.h and lib/gdal>\n"
      "  -DGDAL_DIR=<directory with GDALConfig.cmake>\n"
      "  -DCMAKE_PREFIX_PATH=<install prefix>")
  endif()
  message(STATUS "GDAL: manual search, include=${GDAL_INCLUDE_DIR}, library=${GDAL_LIBRARY}")
  target_include_directories(${NAME}_deps INTERFACE ${GDAL_INCLUDE_DIR})
  target_link_libraries(${NAME}_deps INTERFACE ${GDAL_LIBRARY})
endif()

# --- plog (header-only logging library) ------------------------------------
find_package(plog CONFIG QUIET)

if (TARGET plog::plog)
  message(STATUS "plog: found as imported target plog::plog")
  target_link_libraries(${NAME}_deps INTERFACE plog::plog)
else ()
  find_path(PLOG_INCLUDE_DIR plog/Log.h
    HINTS ${PLOG_ROOT} $ENV{PLOG_ROOT} PATH_SUFFIXES include)
  if (PLOG_INCLUDE_DIR)
    message(STATUS "plog: header-only, include=${PLOG_INCLUDE_DIR}")
    target_include_directories(${NAME}_deps INTERFACE ${PLOG_INCLUDE_DIR})
  else ()
    option(${NAME}_FETCH_PLOG "Download plog from GitHub if it is not installed" OFF)
    if (${NAME}_FETCH_PLOG)
      message(STATUS "plog: downloading from GitHub")
      include(FetchContent)
      FetchContent_Declare(plog
        GIT_REPOSITORY https://github.com/SergiusTheBest/plog.git
        GIT_TAG 1.1.10
        GIT_SHALLOW TRUE)
      FetchContent_MakeAvailable(plog)
      target_link_libraries(${NAME}_deps INTERFACE plog::plog)
    else ()
      message(FATAL_ERROR
        "plog was not found (plog/Log.h).\n"
        "Install it or point CMake at it:\n"
        "  -DPLOG_ROOT=<prefix with include/plog/Log.h>\n"
        "  -DCMAKE_PREFIX_PATH=<install prefix>\n"
        "or let CMake download it: -D${NAME}_FETCH_PLOG=ON")
    endif()
  endif()
endif()

include_directories(${PROJECT_SOURCE_DIR})
message("PROJECT_SOURCE_DIR = ${PROJECT_SOURCE_DIR}\n")

# The algorithm is compiled into both the application and the tests. Keeping it
# as a static library was not worth it: the VS generator does not relink a
# consumer in a subdirectory when the library changes, and a single extra
# compilation of average_data.cpp costs a second.
set(CORE_SOURCES average_data.cpp)

add_executable(${NAME} AverageData_mt.cpp ${CORE_SOURCES})
target_link_libraries(${NAME} PRIVATE ${NAME}_deps Threads::Threads)

# Tests: plain executables registered with CTest, no external framework.
#   cmake --build build && ctest --test-dir build --output-on-failure
include(CTest)
if (BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Note on running the binary: only the executable itself is produced here. With
# vcpkg the GDAL DLLs are copied next to it automatically (applocal). For other
# installations add the GDAL bin directory to PATH (Windows) or rely on the
# runtime search path of the package (Linux/macOS).
