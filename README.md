# AverageData_mt

A small, fast command line tool that averages a stack of single-band rasters
(GeoTIFF) **pixel by pixel** and writes the result as one `Float32` GeoTIFF.

It is used as a helper step in a Stacking-InSAR pipeline: taking a series of
co-registered single-band images (unwrapped phase, coherence, amplitude, ...)
and averaging them to suppress noise and obtain a stable estimate.

## Features

- **NoData aware.** Samples equal to the source NoData value (as well as
  `NaN`/`inf`) are excluded; the divisor is the number of *valid* samples for
  each pixel. Pixels without any valid sample are written as NoData.
- **Double precision accumulation** (per-pixel sum + valid counter), so large
  stacks do not drift even with very different magnitudes.
- **Always Float32 output**, regardless of the input data type (integer
  sources are not truncated).
- **Bounded memory.** The raster is processed in horizontal blocks; the block
  height is chosen so that all worker buffers fit into a 256 MB budget.
- **Multi-threaded.** Files are distributed round-robin over worker threads;
  each thread accumulates into its own buffer, so no locking is needed.
- **Georeferencing preserved** from the first input file (geotransform,
  projection, dataset/band metadata, unit type, scale/offset, NoData).
- **LZW compression** with the floating point predictor (`PREDICTOR=3`).

## Requirements

- C++20 compiler (tested with MSVC 19.44)
- CMake 3.16+
- [GDAL](https://gdal.org/) 3.x (tested with 3.11) — as a CMake package
  (vcpkg, conan, GDAL >= 3.5), through CMake's `FindGDAL` module (OSGeo4W,
  conda, distro packages) or from a plain prefix
- [plog](https://github.com/SergiusTheBest/plog) (header-only logging)

## Build

The project does not hard-code any dependency location. GDAL and plog are
looked up with plain CMake mechanisms, so any installation works.

With vcpkg (either set `VCPKG_ROOT` or pass the toolchain explicitly):

```powershell
$env:VCPKG_ROOT = "C:/vcpkg"
cmake -S . -B build -A x64
cmake --build build --config Release
```

With any other GDAL installation:

```powershell
# prefix that contains include/gdal.h, lib/gdal and include/plog/Log.h
cmake -S . -B build -A x64 -DCMAKE_PREFIX_PATH=C:/OSGeo4W
# or point at GDAL/plog directly
cmake -S . -B build -A x64 -DGDAL_ROOT=C:/gdal -DPLOG_ROOT=C:/plog
# or let CMake download plog if it is not installed
cmake -S . -B build -A x64 -DAverageData_mt_FETCH_PLOG=ON
```

On Linux/macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Usage

```
AverageData_mt -input_file_list <list.txt> -dst <output.tif> [-threads <n>]
```

| Argument | Description |
|---|---|
| `-input_file_list` | text file with one raster path per line (empty lines ignored) |
| `-dst` | output GeoTIFF (created; an existing file is overwritten) |
| `-threads` | number of worker threads, default `4`; clamped to the number of CPU cores and to the number of input files |

```powershell
# inputs.txt
#   D:/data/2024-01-05.unw.tif
#   D:/data/2024-01-17.unw.tif
#   D:/data/2024-01-29.unw.tif

.\AverageData_mt.exe -input_file_list inputs.txt -dst mean.tif -threads 8
```

Progress is printed as a `GDALTermProgress` bar, details go to
`logs/averagedata.log` (rotating, 2 MB × 3 files).

Exit code is `0` on success. If a run fails (for example no readable input),
the return value is non-zero and a partially written output file is removed.

### Files that do not fit

Files that cannot be opened or whose size differs from the first raster are
skipped, the rest is still averaged, and the run reports how many files were
usable:

```
WARN Average was computed over a subset of the input: 8 of 10 files were usable.
```

## Tests

The algorithm is covered by CTest-based tests (no external test framework).
Test rasters are generated in a temporary directory on the fly and deleted
afterwards:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The suite covers the basic average, NoData/NaN exclusion, integer-to-Float32
output, double precision accumulation, block boundaries, skipped/broken
inputs, failure handling, thread clamping and single-file input. It can also
be run directly: `build\tests\Release\test_average.exe`.

## Project layout

```
AverageData_mt.cpp   command line front-end (argument parsing, logging)
average_data.h       public API: AverageOptions, AverageStats, average_data()
average_data.cpp     the averaging core
logger.h             plog initialisation (rotating file log)
tests/               CTest tests + tiny check harness
```

## Notes

- Only band 1 of each input is used; inputs must share the same width/height.
- Only the executable is produced. With vcpkg the GDAL DLLs are copied next to
  it automatically (applocal); otherwise add the GDAL `bin` directory to
  `PATH` (Windows) or rely on the package runtime path (Linux/macOS).

## License

MIT — see [LICENSE](LICENSE).
