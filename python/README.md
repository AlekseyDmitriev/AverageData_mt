# AverageData_mt (Python)

Per-pixel average of a stack of single-band GeoTIFFs — a Python port of the
C++ `AverageData_mt` utility that lives in the repository root.

The semantics are the same as in the C++ version, so the result of both
programs is comparable on the same inputs:

* **NoData/NaN aware.** Samples equal to the source NoData value, `NaN` and
  `±inf` are excluded; the divisor is the number of *valid* samples for each
  pixel. Pixels without any valid sample are written as NoData (`NaN` if the
  sources have no NoData value).
* **float64 accumulation** with a separate valid-sample counter per pixel.
* **Always Float32 output**, LZW compressed with the floating point predictor,
  regardless of the input data type.
* **Bounded memory.** The raster is processed in horizontal blocks; the block
  height is derived from a 256 MB budget unless `block_rows` is given.
* **Georeferencing, CRS, dataset/band metadata, unit, scale/offset and NoData**
  are copied from the first input.
* Files that cannot be opened or whose size differs from the first raster are
  skipped and counted; the rest is still averaged.

## Why it is not just a translation

| | C++ | Python |
|---|---|---|
| Parallelism | `std::async` over file groups | dask graphs, threaded scheduler |
| Reads | `GDALRasterBand::RasterIO` on windows | `rioxarray` (chunked) / `rasterio` (windows) |
| Accumulation | `std::vector<double>` + `uint32_t` | `numpy` float64 + uint32 |
| Progress | `GDALTermProgress` | dask + block counter |

Two engines are available:

* `dask` (default) — each file is opened as a chunked array, per-pixel sums and
  counters are lazy dask graphs, and one block at a time is computed with the
  threaded scheduler. Reads of different files overlap.
* `numpy` — sequential reference implementation, mirrors the C++ block loop.
  Useful to cross-check results and to measure the speed-up of the dask path.

## Install

```powershell
uv venv --python 3.12
uv pip install -e ".[dev]"
```

`uv` is optional; plain `python -m venv .venv` + `pip install -e ".[dev]"` works
too. GDAL comes bundled inside the `rasterio` wheels on Windows, so no separate
GDAL installation is needed.

## Usage

```
average-data-mt -input_file_list <list.txt> -dst <output.tif> [-threads <n>]
```

| Argument | Description |
|---|---|
| `-input_file_list` | text file with one raster path per line (empty lines ignored, CRLF tolerated) |
| `-dst` | output GeoTIFF (created; an existing file is overwritten) |
| `-threads` | worker threads for the dask engine, default `4`, clamped to `[1, number of files]` |
| `--engine` | `dask` (default) or `numpy` |
| `--block-rows` | rows per block; default is chosen from the memory budget |
| `--progress` | print block progress to stderr |
| `-v / -vv` | INFO / DEBUG logging |

```powershell
# inputs.txt
#   D:/data/2024-01-05.unw.tif
#   D:/data/2024-01-17.unw.tif
#   D:/data/2024-01-29.unw.tif

average-data-mt -input_file_list inputs.txt -dst mean.tif -threads 8
# or, without installing the entry point:
python -m average_data_mt -input_file_list inputs.txt -dst mean.tif
```

Exit code is `0` on success. On failure a partially written output file is
removed and the exit code is non-zero.

## Library use

```python
from average_data_mt import AverageOptions, average

stats = average(
    ["a.tif", "b.tif", "c.tif"],
    "mean.tif",
    AverageOptions(threads=8, engine="dask"),
)
print(stats.files_used, stats.blocks)
```

`average()` raises `AverageError` when no result can be produced.

## Tests

```powershell
uv run pytest
```

The suite mirrors the CTest suite of the C++ version (basic average, NoData
exclusion, integer sources, NaN handling, double precision, block boundaries,
skipped/broken inputs, failure handling, thread clamping, single file) and runs
every test once per engine. Test rasters are generated on the fly in pytest's
`tmp_path` and deleted automatically.

Additional tests cover metadata propagation (tags, unit, scale/offset, colour
interpretation), agreement of both engines, agreement with a plain numpy
float64 reference, rioxarray interoperability and the CLI as a subprocess.

## Performance

Measured on a Ryzen 7 2700X (8 cores / 16 threads), 24 LZW-compressed
1024×1024 Float32 rasters (101 MB of input), warm file cache, best of 2 runs:

| engine | threads | time |
|---|---|---|
| numpy (sequential reference) | 1–8 | 1.20 s |
| dask | 1 | 1.61 s |
| dask | 4 | 0.90 s |
| dask | 8 | 0.91 s |
| C++ `AverageData_mt` | 8 | 0.23 s |

The dask speed-up is limited by the workload: reads of LZW-compressed tiles are
dominated by decompression and I/O, so 4 threads are enough and 8 add nothing.
Both Python engines and the C++ binary produce bit-identical outputs on the
same inputs (`max |difference| = 0`).

## Layout

```
src/average_data_mt/
  model.py      AverageOptions, AverageStats, AverageError
  raster.py     metadata, validation, windowed reads, output creation
  engines.py    numpy (reference) and dask (parallel) block accumulators
  core.py       orchestration: validation -> blocks -> write
  cli.py        argument parsing, file list, logging
tests/
  _rasters.py   helpers to build and read rasters
  test_average.py   algorithm tests (both engines)
  test_metadata.py  metadata, engine equivalence, rioxarray interop
  test_cli.py       command line tests
```

## Limitations

* Only band 1 of each input is used.
* The result is materialised one block at a time, but the final block still
  needs `block_rows * width * 4` bytes; the dask graph also keeps the per-file
  arrays of the current block only.
* No CRS transformation, resampling or reprojection — inputs must already be
  co-registered, exactly like the C++ version.

## License

MIT.
