// average_data.cpp
//
// Implementation of the averaging core. See average_data.h for the summary.

#include "average_data.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <vector>

#include <cpl_error.h>
#include <cpl_port.h>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <plog/Log.h>

namespace {

// Memory budget for one block (all worker buffers + the merged block buffers).
constexpr size_t BLOCK_MEMORY_BUDGET_BYTES = 256ull * 1024 * 1024;

// Number of file reads performed so far (all threads, all blocks).
std::atomic<size_t> PROCESSED_FILES{ 0 };
// Total number of file reads the run consists of (files * blocks).
size_t TOTAL_FILES{ 0 };

// Result of averaging one group of files over one horizontal block.
struct BlockResult {
    std::vector<double> sum;     // per-pixel sum of valid samples (block-local)
    std::vector<uint32_t> count; // per-pixel number of valid samples
    size_t processed = 0;        // files successfully read
    size_t failed_reads = 0;     // files that could not be opened/read
    size_t dimension_mismatches = 0;
};

// A sample participates in the average only if it is a finite number that is
// not equal to the NoData value of the source rasters.
bool is_valid_value(float value, bool has_no_data, double no_data_value)
{
    if (!std::isfinite(value)) {
        return false;
    }
    if (!has_no_data) {
        return true;
    }

    // Compare both in double (as read) and in float (as stored) to avoid
    // missing the NoData value because of float -> double conversion.
    return !(static_cast<double>(value) == no_data_value ||
        value == static_cast<float>(no_data_value));
}

// Sums one group of files over the block [y_offset, y_offset + block_rows).
BlockResult process_block_group(const std::vector<std::string>& file_group, int x_size, int y_size,
                                int y_offset, int block_rows, bool has_no_data, double no_data_value)
{
    const size_t block_pixels = static_cast<size_t>(x_size) * static_cast<size_t>(block_rows);

    BlockResult result;
    result.sum.assign(block_pixels, 0.0);
    result.count.assign(block_pixels, 0u);

    std::vector<float> buffer(block_pixels);

    for (const auto& filename : file_group) {
        GDALDataset* poDS = (GDALDataset*)GDALOpen(filename.c_str(), GA_ReadOnly);
        if (!poDS) {
            PLOG_ERROR << "Error reading file: " << filename << "!";
            ++result.failed_reads;
            continue;
        }

        int curXSize = poDS->GetRasterXSize();
        int curYSize = poDS->GetRasterYSize();
        if ((curXSize != x_size) || (curYSize != y_size)) {
            PLOG_ERROR << std::format("Raster dimension of \"{}\" doesn't equal to first image! Current image size is {}x{}.",
                filename, curXSize, curYSize);
            GDALClose((GDALDatasetH)poDS);
            ++result.dimension_mismatches;
            continue;
        }

        GDALRasterBand* poBand = poDS->GetRasterBand(1);
        if (!poBand) {
            PLOG_ERROR << std::format("File \"{}\" has no raster band 1!", filename);
            GDALClose((GDALDatasetH)poDS);
            ++result.failed_reads;
            continue;
        }

        CPLErr errCode = poBand->RasterIO(GF_Read, 0, y_offset, x_size, block_rows,
            buffer.data(), x_size, block_rows, GDT_Float32, 0, 0);

        if (errCode != CPLErr::CE_None) {
            PLOG_ERROR << std::format("Error reading raster data from file: {}!", filename);
            GDALClose((GDALDatasetH)poDS);
            ++result.failed_reads;
            continue;
        }

        // Accumulate only valid samples, remembering how many there were.
        for (size_t i = 0; i < block_pixels; ++i) {
            const float value = buffer[i];
            if (!is_valid_value(value, has_no_data, no_data_value)) {
                continue;
            }
            result.sum[i] += static_cast<double>(value);
            ++result.count[i];
        }

        ++result.processed;
        PROCESSED_FILES.fetch_add(1, std::memory_order_relaxed);
        GDALClose((GDALDatasetH)poDS);
    }

    return result;
}

// Splits the file list into per-thread groups (round-robin).
std::vector<std::vector<std::string>> distribute_files(const std::vector<std::string>& src_files, int num_threads)
{
    std::vector<std::vector<std::string>> groups(num_threads);

    for (size_t i = 0; i < src_files.size(); ++i) {
        const int group_index = static_cast<int>(i % static_cast<size_t>(num_threads));
        groups[group_index].push_back(src_files[i]);
    }
    return groups;
}

// Reads the NoData value of a band with a GDAL 3.11 compatible API.
bool read_no_data_value(GDALRasterBand* band, double& no_data_value)
{
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 11, 0)
    int no_data_flag = FALSE;
    no_data_value = band->GetNoDataValue(&no_data_flag);
    return no_data_flag == TRUE;
#else
    return band->GetNoDataValue(&no_data_value) != nullptr;
#endif
}

// Applies one source block height (0 => automatic) to the given raster width.
int resolve_block_rows(int requested_rows, int x_size, int y_size, int num_threads)
{
    if (requested_rows > 0) {
        return std::min(requested_rows, y_size);
    }

    // Keep (threads + 1) working buffers inside the budget.
    const size_t bytes_per_row = static_cast<size_t>(x_size) *
        (sizeof(double) + sizeof(uint32_t) + sizeof(float)) *
        static_cast<size_t>(num_threads + 1);
    const int rows = static_cast<int>(std::max<size_t>(1, BLOCK_MEMORY_BUDGET_BYTES / std::max<size_t>(1, bytes_per_row)));
    return std::min(rows, y_size);
}

} // namespace

int average_data(const std::vector<std::string>& src_files,
                 const std::string& dst_filename,
                 const AverageOptions& options,
                 AverageStats* stats)
{
    PLOG_INFO << "Starting average_data(...)";

    if (stats) {
        *stats = AverageStats();
        stats->files_total = src_files.size();
    }

    if (src_files.empty()) {
        PLOG_ERROR << "No input files provided!";
        return -1;
    }

    const int num_threads = std::max(1, std::min(options.threads, static_cast<int>(src_files.size())));
    PLOG_INFO << std::format("Processing {} input files with {} threads", src_files.size(), num_threads);

    // Opening the first file to get dimensions, georeferencing and NoData.
    GDALDataset* poSrcDS = (GDALDataset*)GDALOpen(src_files[0].c_str(), GA_ReadOnly);
    if (!poSrcDS) {
        PLOG_ERROR << std::format("Error reading file {}! Exiting.", src_files[0]);
        return -1;
    }

    const int xSize = poSrcDS->GetRasterXSize();
    const int ySize = poSrcDS->GetRasterYSize();
    GDALRasterBand* poSrcBand = poSrcDS->GetRasterBand(1);
    if (!poSrcBand || xSize <= 0 || ySize <= 0) {
        PLOG_ERROR << std::format("File {} has no usable raster band 1 ({}x{})! Exiting.", src_files[0], xSize, ySize);
        GDALClose((GDALDatasetH)poSrcDS);
        return -1;
    }

    double no_data_value = 0.0;
    const bool has_no_data = read_no_data_value(poSrcBand, no_data_value);
    if (has_no_data) {
        PLOG_INFO << std::format("NoData value of the source rasters: {}", no_data_value);
    }
    else {
        PLOG_WARNING << "Source rasters have no NoData value. NaN/inf pixels are treated as invalid, "
            "and pixels without valid samples are written as NaN.";
    }
    PLOG_INFO << std::format("First image size is {}x{}", xSize, ySize);

    // Output is always Float32, independently of the source data type.
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!poDriver) {
        PLOG_ERROR << "GTiff driver is not available! Exiting.";
        GDALClose((GDALDatasetH)poSrcDS);
        return -1;
    }

    char** papszCreationOptions = nullptr;
    papszCreationOptions = CSLAddString(papszCreationOptions, "COMPRESS=LZW");
    papszCreationOptions = CSLAddString(papszCreationOptions, "PREDICTOR=3");

    GDALDataset* poDstDS = poDriver->Create(dst_filename.c_str(), xSize, ySize, 1, GDT_Float32, papszCreationOptions);
    CSLDestroy(papszCreationOptions);

    if (!poDstDS) {
        PLOG_ERROR << std::format("Error creating output file {}! Exiting.", dst_filename);
        GDALClose((GDALDatasetH)poSrcDS);
        return -1;
    }

    // Preserve georeferencing / metadata of the first image.
    double adfGeoTransform[6] = { 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    if (poSrcDS->GetGeoTransform(adfGeoTransform) == CE_None) {
        poDstDS->SetGeoTransform(adfGeoTransform);
    }
    const char* pszProjection = poSrcDS->GetProjectionRef();
    if (pszProjection && *pszProjection) {
        poDstDS->SetProjection(pszProjection);
    }
    poDstDS->SetMetadata(poSrcDS->GetMetadata());

    GDALRasterBand* poDstBand = poDstDS->GetRasterBand(1);
    poDstBand->SetMetadata(poSrcBand->GetMetadata());
    poDstBand->SetColorInterpretation(poSrcBand->GetColorInterpretation());
    if (const char* pszUnit = poSrcBand->GetUnitType(); pszUnit && *pszUnit) {
        poDstBand->SetUnitType(pszUnit);
    }

    int has_scale = FALSE;
    const double src_scale = poSrcBand->GetScale(&has_scale);
    if (has_scale) {
        poDstBand->SetScale(src_scale);
    }
    int has_offset = FALSE;
    const double src_offset = poSrcBand->GetOffset(&has_offset);
    if (has_offset) {
        poDstBand->SetOffset(src_offset);
    }

    GDALClose((GDALDatasetH)poSrcDS); // No need anymore
    poSrcDS = nullptr;
    poSrcBand = nullptr;

    const int block_rows = resolve_block_rows(options.block_rows, xSize, ySize, num_threads);
    const size_t block_count = (static_cast<size_t>(ySize) + static_cast<size_t>(block_rows) - 1) /
        static_cast<size_t>(block_rows);
    PLOG_INFO << std::format("Processing in {} block(s) of up to {} row(s).", block_count, block_rows);

    const std::vector<std::vector<std::string>> file_groups = distribute_files(src_files, num_threads);
    for (size_t i = 0; i < file_groups.size(); ++i) {
        PLOG_INFO << std::format("Group {}: {} files", i, file_groups[i].size());
    }

    PROCESSED_FILES.store(0, std::memory_order_relaxed);
    TOTAL_FILES = src_files.size() * block_count;

    const float fill_value = has_no_data ? static_cast<float>(no_data_value)
        : std::numeric_limits<float>::quiet_NaN();

    size_t processed_reads = 0;
    size_t failed_reads = 0;
    size_t dimension_mismatches = 0;

    for (int y_offset = 0; y_offset < ySize; y_offset += block_rows) {
        const int rows = std::min(block_rows, ySize - y_offset);
        const size_t block_pixels = static_cast<size_t>(xSize) * static_cast<size_t>(rows);

        std::vector<std::future<BlockResult>> futures;
        futures.reserve(file_groups.size());
        for (const auto& group : file_groups) {
            if (group.empty()) {
                continue;
            }
            futures.push_back(std::async(std::launch::async, process_block_group,
                std::cref(group), xSize, ySize, y_offset, rows, has_no_data, no_data_value));
        }

        std::vector<double> block_sum(block_pixels, 0.0);
        std::vector<uint32_t> block_count(block_pixels, 0u);

        for (auto& future : futures) {
            BlockResult part = future.get();

            for (size_t i = 0; i < block_pixels; ++i) {
                block_sum[i] += part.sum[i];
                block_count[i] += part.count[i];
            }

            processed_reads += part.processed;
            failed_reads += part.failed_reads;
            dimension_mismatches += part.dimension_mismatches;
        }

        // Per-pixel average over valid samples only.
        std::vector<float> block_avg(block_pixels, fill_value);
        for (size_t i = 0; i < block_pixels; ++i) {
            if (block_count[i] > 0) {
                block_avg[i] = static_cast<float>(block_sum[i] / static_cast<double>(block_count[i]));
            }
        }

        PLOG_INFO << std::format("Writing rows {}-{} of {}.", y_offset, y_offset + rows - 1, ySize);
        CPLErr errCode = poDstBand->RasterIO(GF_Write, 0, y_offset, xSize, rows,
            block_avg.data(), xSize, rows, GDT_Float32, 0, 0);
        if (errCode != CPLErr::CE_None) {
            PLOG_ERROR << std::format("Error writing data to output file: {}!", dst_filename);
            PLOG_ERROR << std::format("Error message: {}", CPLGetLastErrorMsg());
            const int error_no = CPLGetLastErrorNo();
            GDALClose((GDALDatasetH)poDstDS);
            VSIUnlink(dst_filename.c_str());
            PLOG_ERROR << "Incomplete output file removed.";
            return error_no;
        }

        // The progress bar is updated from the main thread only (thread-safe).
        if (options.progress) {
            GDALTermProgress(static_cast<double>(PROCESSED_FILES.load(std::memory_order_relaxed)) /
                static_cast<double>(std::max<size_t>(1, TOTAL_FILES)), nullptr, nullptr);
        }
    }

    PLOG_INFO << std::format("All blocks completed. Average computed from {} file read(s); "
        "{} read failure(s), {} dimension mismatch(es).",
        processed_reads, failed_reads, dimension_mismatches);

    if (processed_reads == 0) {
        PLOG_ERROR << "No files were processed successfully!";
        GDALClose((GDALDatasetH)poDstDS);
        VSIUnlink(dst_filename.c_str());
        return -1;
    }

    if (failed_reads > 0 || dimension_mismatches > 0) {
        PLOG_WARNING << std::format("Average was computed over a subset of the input: "
            "{} of {} files were usable.", processed_reads / std::max<size_t>(1, block_count), src_files.size());
    }

    if (stats) {
        stats->files_used = processed_reads / std::max<size_t>(1, block_count);
        stats->failed_reads = failed_reads;
        stats->dimension_mismatches = dimension_mismatches;
        stats->blocks = static_cast<int>(block_count);
    }

    poDstBand->SetNoDataValue(has_no_data ? no_data_value : std::numeric_limits<double>::quiet_NaN());

    PLOG_INFO << "Cleaning up.";
    GDALClose((GDALDatasetH)poDstDS);
    if (options.progress) {
        GDALTermProgress(1.0, nullptr, nullptr);
    }
    PLOG_INFO << "Exiting average_data().";
    return 0;
}
