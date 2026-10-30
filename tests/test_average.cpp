// test_average.cpp
//
// Self-contained tests for the averaging core (average_data.h).
//
// Rationale: the program is small, so a full-blown framework (GoogleTest,
// Catch2, ...) would only add a dependency. All tests are plain functions with
// a few macros, and CTest (built into CMake) runs this executable: `ctest`.
//
// Test rasters are generated on the fly in a unique temporary directory and
// deleted when the fixture goes out of scope.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <cpl_error.h>
#include <gdal.h>
#include <gdal_priv.h>

#include "average_data.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Tiny check helpers
// ---------------------------------------------------------------------------
static int g_failures = 0;

static void report_failure(const char* file, int line, const std::string& message)
{
    ++g_failures;
    printf("  FAIL %s:%d: %s\n", file, line, message.c_str());
}

static bool check_bool(bool condition, const char* expr, const char* file, int line)
{
    if (!condition) {
        report_failure(file, line, std::string("expected to be true: ") + expr);
    }
    return condition;
}

template <class A, class B>
static bool check_eq(const A& actual, const B& expected, const char* expr, const char* file, int line)
{
    if (actual == expected) {
        return true;
    }
    std::ostringstream oss;
    oss << expr << " == " << expected << ", actual " << actual;
    report_failure(file, line, oss.str());
    return false;
}

static bool check_near(double actual, double expected, double eps, const char* expr, const char* file, int line)
{
    if (std::isfinite(actual) && std::fabs(actual - expected) <= eps) {
        return true;
    }
    std::ostringstream oss;
    oss.precision(12);
    oss << expr << " ~= " << expected << " (eps " << eps << "), actual " << actual;
    report_failure(file, line, oss.str());
    return false;
}

#define CHECK(expr) check_bool((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) check_eq((actual), (expected), #actual, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, eps) check_near((actual), (expected), (eps), #actual, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Fixtures: a temporary directory with generated rasters
// ---------------------------------------------------------------------------
class TempDir {
public:
    explicit TempDir(const std::string& tag)
    {
        std::random_device rd;
        dir_ = fs::temp_directory_path() / std::format("averagedata_test_{}_{}", tag, rd());
        fs::create_directories(dir_);
    }

    // Rasters (and everything else created inside) are removed here.
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string path(const std::string& name) const { return (dir_ / name).string(); }

private:
    fs::path dir_;
};

static const double TEST_NODATA = -9999.0;

// Silences GDAL's stderr messages for expected errors (missing files etc.).
class QuietGdalErrors {
public:
    QuietGdalErrors() { CPLPushErrorHandler(CPLQuietErrorHandler); }
    ~QuietGdalErrors() { CPLPopErrorHandler(); }
    QuietGdalErrors(const QuietGdalErrors&) = delete;
    QuietGdalErrors& operator=(const QuietGdalErrors&) = delete;
};

// Writes a single-band GeoTIFF from floating point values (GDAL converts them
// to `type`). Georeferencing and projection are set so that copying them to the
// output can be verified.
static void create_raster(const std::string& path, int width, int height,
                          const std::vector<double>& values, GDALDataType type,
                          bool has_nodata = false, double nodata = TEST_NODATA)
{
    if (values.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
        report_failure(__FILE__, __LINE__, "test bug: wrong number of values");
        return;
    }

    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset* ds = driver->Create(path.c_str(), width, height, 1, type, nullptr);
    if (!ds) {
        report_failure(__FILE__, __LINE__, "cannot create " + path);
        return;
    }

    std::vector<double> buffer = values;
    GDALRasterBand* band = ds->GetRasterBand(1);
    if (band->RasterIO(GF_Write, 0, 0, width, height, buffer.data(), width, height,
            GDT_Float64, 0, 0) != CE_None) {
        report_failure(__FILE__, __LINE__, "cannot write " + path);
    }
    if (has_nodata) {
        band->SetNoDataValue(nodata);
    }

    double geo_transform[6] = { 1000.0, 10.0, 0.0, 2000.0, 0.0, -10.0 };
    ds->SetGeoTransform(geo_transform);
    ds->SetProjection("LOCAL_CS[\"test\"]");

    GDALClose((GDALDatasetH)ds);
}

static std::string constant_raster(const TempDir& dir, const std::string& name, int width, int height,
                                   double value, GDALDataType type = GDT_Float32,
                                   bool has_nodata = false, double nodata = TEST_NODATA)
{
    create_raster(dir.path(name), width, height,
        std::vector<double>(static_cast<size_t>(width) * static_cast<size_t>(height), value),
        type, has_nodata, nodata);
    return dir.path(name);
}

struct Raster {
    int width = 0;
    int height = 0;
    GDALDataType type = GDT_Unknown;
    bool has_nodata = false;
    double nodata = 0.0;
    double geo_transform[6] = {};
    std::string projection;
    std::vector<float> values;
};

static bool read_raster(const std::string& path, Raster& out)
{
    GDALDataset* ds = (GDALDataset*)GDALOpen(path.c_str(), GA_ReadOnly);
    if (!ds) {
        report_failure(__FILE__, __LINE__, "cannot open " + path);
        return false;
    }

    out.width = ds->GetRasterXSize();
    out.height = ds->GetRasterYSize();

    GDALRasterBand* band = ds->GetRasterBand(1);
    out.type = band->GetRasterDataType();
    out.values.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 0.0f);
    if (band->RasterIO(GF_Read, 0, 0, out.width, out.height, out.values.data(),
            out.width, out.height, GDT_Float32, 0, 0) != CE_None) {
        report_failure(__FILE__, __LINE__, "cannot read pixels of " + path);
        GDALClose((GDALDatasetH)ds);
        return false;
    }

    // GetNoDataValue() takes an `int*` success flag in every GDAL version.
    int no_data_flag = FALSE;
    out.nodata = band->GetNoDataValue(&no_data_flag);
    out.has_nodata = no_data_flag == TRUE;

    ds->GetGeoTransform(out.geo_transform);
    const char* projection = ds->GetProjectionRef();
    out.projection = projection ? projection : "";

    GDALClose((GDALDatasetH)ds);
    return true;
}

// Safe pixel accessor: out-of-range reads return NaN instead of crashing.
static float pixel(const Raster& raster, int x, int y)
{
    if (x < 0 || y < 0 || x >= raster.width || y >= raster.height) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return raster.values[static_cast<size_t>(y) * static_cast<size_t>(raster.width) + x];
}

static AverageOptions test_options(int threads, int block_rows = 0)
{
    AverageOptions options;
    options.threads = threads;
    options.block_rows = block_rows;
    options.progress = false; // keep the test output clean
    return options;
}

// ---------------------------------------------------------------------------
// 1. Basic average, output format and copied georeferencing
// ---------------------------------------------------------------------------
static void test_basic_average()
{
    TempDir dir("basic");
    const int width = 4;
    const int height = 3;

    const std::vector<std::string> files{
        constant_raster(dir, "a.tif", width, height, 10.0, GDT_Float32, true),
        constant_raster(dir, "b.tif", width, height, 20.0, GDT_Float32, true),
        constant_raster(dir, "c.tif", width, height, 30.0, GDT_Float32, true),
    };

    const std::string dst = dir.path("out.tif");
    AverageStats stats;
    const int rc = average_data(files, dst, test_options(3), &stats);

    CHECK_EQ(rc, 0);
    CHECK_EQ(stats.files_total, static_cast<size_t>(3));
    CHECK_EQ(stats.files_used, static_cast<size_t>(3));
    CHECK_EQ(stats.failed_reads, static_cast<size_t>(0));
    CHECK_EQ(stats.dimension_mismatches, static_cast<size_t>(0));
    CHECK_EQ(stats.blocks, 1);

    Raster out;
    if (!CHECK(read_raster(dst, out))) {
        return;
    }

    CHECK_EQ(out.width, width);
    CHECK_EQ(out.height, height);
    CHECK(out.type == GDT_Float32);
    CHECK_EQ(out.values.size(), static_cast<size_t>(width) * height);
    for (float value : out.values) {
        CHECK_NEAR(value, 20.0, 1e-6);
    }

    CHECK(out.has_nodata);
    CHECK_NEAR(out.nodata, TEST_NODATA, 1e-9);
    CHECK_NEAR(out.geo_transform[0], 1000.0, 1e-9);
    CHECK_NEAR(out.geo_transform[1], 10.0, 1e-9);
    CHECK_NEAR(out.geo_transform[3], 2000.0, 1e-9);
    CHECK_NEAR(out.geo_transform[5], -10.0, 1e-9);
    CHECK(!out.projection.empty());
}

// ---------------------------------------------------------------------------
// 2. NoData samples are excluded and the divisor is the per-pixel valid count
// ---------------------------------------------------------------------------
static void test_nodata_is_excluded()
{
    TempDir dir("nodata");
    const int width = 3;
    const int height = 2;

    std::vector<double> a(6, 10.0);
    a[0] = TEST_NODATA;  // (0,0)
    a[5] = TEST_NODATA;  // (2,1)

    std::vector<double> b(6, 20.0);
    b[0] = TEST_NODATA;  // (0,0)

    std::vector<double> c(6, 30.0); // no holes

    create_raster(dir.path("a.tif"), width, height, a, GDT_Float32, true);
    create_raster(dir.path("b.tif"), width, height, b, GDT_Float32, true);
    create_raster(dir.path("c.tif"), width, height, c, GDT_Float32, true);

    const std::string dst = dir.path("out.tif");
    AverageStats stats;
    CHECK_EQ(average_data({ dir.path("a.tif"), dir.path("b.tif"), dir.path("c.tif") },
        dst, test_options(2), &stats), 0);

    Raster out;
    if (!CHECK(read_raster(dst, out))) {
        return;
    }

    // (0,0): 10 and 20 are NoData -> only 30 remains.
    CHECK_NEAR(pixel(out, 0, 0), 30.0, 1e-6);
    // (2,1): only file a is NoData -> (20 + 30) / 2.
    CHECK_NEAR(pixel(out, 2, 1), 25.0, 1e-6);
    // (1,0): all three samples are valid -> (10 + 20 + 30) / 3.
    CHECK_NEAR(pixel(out, 1, 0), 20.0, 1e-6);
    CHECK_NEAR(pixel(out, 1, 1), 20.0, 1e-6);
}

// ---------------------------------------------------------------------------
// 3. Integer sources still produce a Float32 result (no truncation)
// ---------------------------------------------------------------------------
static void test_int16_sources_are_not_truncated()
{
    TempDir dir("int16");
    const std::vector<std::string> files{
        constant_raster(dir, "a.tif", 2, 1, 1.0, GDT_Int16),
        constant_raster(dir, "b.tif", 2, 1, 2.0, GDT_Int16),
    };

    const std::string dst = dir.path("out.tif");
    CHECK_EQ(average_data(files, dst, test_options(2)), 0);

    Raster out;
    if (!CHECK(read_raster(dst, out))) {
        return;
    }

    CHECK(out.type == GDT_Float32);
    CHECK_NEAR(pixel(out, 0, 0), 1.5, 1e-6);
    CHECK_NEAR(pixel(out, 1, 0), 1.5, 1e-6);
}

// ---------------------------------------------------------------------------
// 4. NaN is treated as an invalid sample; uncovered pixels become NoData
// ---------------------------------------------------------------------------
static void test_nan_is_excluded()
{
    TempDir dir("nan");
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // No NoData value is defined for the sources on purpose.
    create_raster(dir.path("a.tif"), 2, 2, { 10.0, nan, nan, 10.0 }, GDT_Float32, false);
    create_raster(dir.path("b.tif"), 2, 2, { 20.0, 20.0, nan, nan }, GDT_Float32, false);

    const std::string dst = dir.path("out.tif");
    CHECK_EQ(average_data({ dir.path("a.tif"), dir.path("b.tif") }, dst, test_options(2)), 0);

    Raster out;
    if (!CHECK(read_raster(dst, out))) {
        return;
    }

    CHECK_NEAR(pixel(out, 0, 0), 15.0, 1e-6);                 // 10 and 20
    CHECK_NEAR(pixel(out, 1, 0), 20.0, 1e-6);                 // NaN and 20
    CHECK(std::isnan(pixel(out, 0, 1)));                      // NaN and NaN -> no data
    CHECK_NEAR(pixel(out, 1, 1), 10.0, 1e-6);                 // 10 and NaN

    CHECK(out.has_nodata);
    CHECK(std::isnan(out.nodata));
}

// ---------------------------------------------------------------------------
// 5. Accumulation is done in double precision
// ---------------------------------------------------------------------------
static void test_double_precision_accumulation()
{
    TempDir dir("precision");
    const int file_count = 100;

    std::vector<std::string> files;
    files.reserve(file_count);
    files.push_back(constant_raster(dir, "big.tif", 1, 1, 1e8));
    for (int i = 1; i < file_count; ++i) {
        files.push_back(constant_raster(dir, std::format("small_{}.tif", i), 1, 1, 1.0));
    }

    const std::string dst = dir.path("out.tif");
    // A single thread makes the float-vs-double difference deterministic:
    // summing in float would swallow all 99 small values (ulp of 1e8 is 8).
    CHECK_EQ(average_data(files, dst, test_options(1)), 0);

    Raster out;
    if (!CHECK(read_raster(dst, out))) {
        return;
    }

    const double exact = (1e8 + (file_count - 1)) / file_count;
    const double stored = static_cast<double>(static_cast<float>(exact));
    CHECK_NEAR(pixel(out, 0, 0), stored, 1e-3);
    CHECK_NEAR(pixel(out, 0, 0), exact, 0.05);
}

// ---------------------------------------------------------------------------
// 6. The same result regardless of the block height (incl. block boundary)
// ---------------------------------------------------------------------------
static void test_block_boundaries()
{
    TempDir dir("blocks");
    const int width = 2;
    const int height = 5;

    std::vector<double> a(static_cast<size_t>(width) * height, 10.0);
    a[3 * width + 1] = TEST_NODATA; // (1,3) -> hole in the middle of the raster

    std::vector<double> b(static_cast<size_t>(width) * height, 20.0);
    b[3 * width + 1] = TEST_NODATA;

    create_raster(dir.path("a.tif"), width, height, a, GDT_Float32, true);
    create_raster(dir.path("b.tif"), width, height, b, GDT_Float32, true);
    create_raster(dir.path("c.tif"), width, height,
        std::vector<double>(static_cast<size_t>(width) * height, 30.0), GDT_Float32, true);

    const std::vector<std::string> files{ dir.path("a.tif"), dir.path("b.tif"), dir.path("c.tif") };

    const std::vector<std::pair<int, int>> cases{ { 1, 5 }, { 2, 3 }, { 4, 2 }, { 5, 1 } };
    for (const auto& [block_rows, expected_blocks] : cases) {
        const std::string dst = dir.path(std::format("out_{}.tif", block_rows));
        AverageStats stats;
        CHECK_EQ(average_data(files, dst, test_options(2, block_rows), &stats), 0);
        CHECK_EQ(stats.blocks, expected_blocks);

        Raster out;
        if (!CHECK(read_raster(dst, out))) {
            continue;
        }

        CHECK_EQ(out.width, width);
        CHECK_EQ(out.height, height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool hole = (x == 1 && y == 3);
                CHECK_NEAR(pixel(out, x, y), hole ? 30.0 : 20.0, 1e-6);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 7. Unreadable and mismatched files are skipped, the rest is still averaged
// ---------------------------------------------------------------------------
static void test_bad_inputs_are_skipped()
{
    const QuietGdalErrors quiet;
    TempDir dir("skips");
    const int width = 3;
    const int height = 2;

    const std::string good_a = constant_raster(dir, "good_a.tif", width, height, 10.0);
    const std::string good_b = constant_raster(dir, "good_b.tif", width, height, 20.0);
    const std::string wrong_size = constant_raster(dir, "wrong_size.tif", width + 1, height, 1000.0);
    const std::string missing = dir.path("does_not_exist.tif");

    const std::string dst = dir.path("out.tif");
    AverageStats stats;
    const int rc = average_data({ good_a, missing, wrong_size, good_b }, dst, test_options(2), &stats);

    CHECK_EQ(rc, 0);
    CHECK_EQ(stats.files_total, static_cast<size_t>(4));
    CHECK_EQ(stats.files_used, static_cast<size_t>(2));
    CHECK_EQ(stats.failed_reads, static_cast<size_t>(1));
    CHECK_EQ(stats.dimension_mismatches, static_cast<size_t>(1));

    Raster out;
    if (!CHECK(read_raster(dst, out))) {
        return;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            CHECK_NEAR(pixel(out, x, y), 15.0, 1e-6);
        }
    }
}

// ---------------------------------------------------------------------------
// 8. Failing runs produce no output file
// ---------------------------------------------------------------------------
static void test_failures_leave_no_output()
{
    const QuietGdalErrors quiet;
    TempDir dir("failures");

    const std::string dst_all_bad = dir.path("out_all_bad.tif");
    CHECK(average_data({ dir.path("missing1.tif"), dir.path("missing2.tif") },
        dst_all_bad, test_options(2)) != 0);
    CHECK(!fs::exists(dst_all_bad));

    const std::string dst_empty = dir.path("out_empty.tif");
    CHECK(average_data({}, dst_empty, test_options(2)) != 0);
    CHECK(!fs::exists(dst_empty));
}

// ---------------------------------------------------------------------------
// 9. Thread count is clamped to sane values
// ---------------------------------------------------------------------------
static void test_thread_count_is_clamped()
{
    TempDir dir("threads");
    const std::vector<std::string> files{
        constant_raster(dir, "a.tif", 2, 2, 10.0),
        constant_raster(dir, "b.tif", 2, 2, 20.0),
    };

    for (int threads : { 0, -7, 1, 64 }) {
        const std::string dst = dir.path(std::format("out_{}.tif", threads));
        AverageStats stats;
        CHECK_EQ(average_data(files, dst, test_options(threads), &stats), 0);
        CHECK_EQ(stats.files_used, static_cast<size_t>(2));

        Raster out;
        if (!CHECK(read_raster(dst, out))) {
            continue;
        }
        CHECK_NEAR(pixel(out, 0, 0), 15.0, 1e-6);
    }
}

// ---------------------------------------------------------------------------
// 10. Single file input (average equals the input)
// ---------------------------------------------------------------------------
static void test_single_file()
{
    TempDir dir("single");
    create_raster(dir.path("only.tif"), 2, 1, { 7.5, 8.5 }, GDT_Float32);

    const std::string dst = dir.path("out.tif");
    CHECK_EQ(average_data({ dir.path("only.tif") }, dst, test_options(8)), 0);

    Raster out;
    if (!CHECK(read_raster(dst, out))) {
        return;
    }
    CHECK_NEAR(pixel(out, 0, 0), 7.5, 1e-6);
    CHECK_NEAR(pixel(out, 1, 0), 8.5, 1e-6);
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------
int main()
{
    GDALAllRegister();

    struct TestCase {
        const char* name;
        void (*run)();
    };

    const TestCase tests[] = {
        { "basic_average", test_basic_average },
        { "nodata_is_excluded", test_nodata_is_excluded },
        { "int16_sources_are_not_truncated", test_int16_sources_are_not_truncated },
        { "nan_is_excluded", test_nan_is_excluded },
        { "double_precision_accumulation", test_double_precision_accumulation },
        { "block_boundaries", test_block_boundaries },
        { "bad_inputs_are_skipped", test_bad_inputs_are_skipped },
        { "failures_leave_no_output", test_failures_leave_no_output },
        { "thread_count_is_clamped", test_thread_count_is_clamped },
        { "single_file", test_single_file },
    };

    int passed = 0;
    const int total = static_cast<int>(std::size(tests));

    for (const auto& test : tests) {
        const int failures_before = g_failures;
        printf("[ RUN      ] %s\n", test.name);
        test.run();

        if (g_failures == failures_before) {
            ++passed;
            printf("[       OK ] %s\n", test.name);
        }
        else {
            printf("[  FAILED  ] %s (%d failed check(s))\n", test.name, g_failures - failures_before);
        }
    }

    printf("\n%d/%d test(s) passed, %d failed check(s)\n", passed, total, g_failures);

    GDALDestroyDriverManager();
    return g_failures == 0 ? 0 : 1;
}
