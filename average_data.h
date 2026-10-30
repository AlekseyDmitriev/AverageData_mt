// average_data.h
//
// Core of AverageData_mt: per-pixel averaging of a stack of single-band rasters.
//
// Key properties:
//   * NoData aware: pixels equal to the source NoData value (and NaN/inf) are
//     excluded from the average, the divisor is the number of valid samples
//     per pixel. Pixels with no valid samples are written as NoData.
//   * Accumulation is done in double precision (per-pixel sum + valid counter).
//   * The output band is always Float32 (never inherits an integer type from
//     the first input file).
//   * Raster is processed in horizontal blocks, so memory usage stays bounded
//     regardless of image size or thread count.

#ifndef AVERAGE_DATA_H
#define AVERAGE_DATA_H

#include <cstddef>
#include <string>
#include <vector>

// Options of a single averaging run.
struct AverageOptions {
    int threads = 4;      // worker threads, clamped to [1, number of files]
    int block_rows = 0;   // rows per block; 0 => chosen automatically from the memory budget
    bool progress = true; // show GDALTermProgress on stdout
};

// Counters collected during a run (used for logging and for tests).
struct AverageStats {
    std::size_t files_total = 0;            // files passed in
    std::size_t files_used = 0;             // files that contributed at least one read
    std::size_t failed_reads = 0;           // files that could not be opened/read
    std::size_t dimension_mismatches = 0;   // files skipped because of a different size
    int blocks = 0;                         // number of processed blocks
};

// Averages all `files` pixel by pixel and writes the result to `dst_filename`
// as a Float32 GeoTIFF (LZW). Georeferencing, metadata and NoData are taken
// from the first file.
//
// Returns 0 on success, a non-zero value on error; a partially written output
// file is removed in that case.
int average_data(const std::vector<std::string>& files,
                 const std::string& dst_filename,
                 const AverageOptions& options = AverageOptions(),
                 AverageStats* stats = nullptr);

#endif // AVERAGE_DATA_H
