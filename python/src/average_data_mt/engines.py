"""Block accumulators.

Both engines produce the same result; they differ only in how the per-file
reads of a block are executed:

* ``numpy`` - one file after another, plain numpy accumulation. This is the
  reference implementation and mirrors the C++ block loop one to one.
* ``dask`` - every file is opened with rioxarray as a chunked (dask) array; the
  per-pixel sums and valid counters are built as lazy dask graphs and computed
  with the threaded scheduler, so reads of different files overlap. Only one
  block is materialised at a time, so memory stays bounded.

Accumulation is always done in float64 with a separate valid-sample counter,
so a pixel is divided by the number of samples that actually exist for it.
"""

from __future__ import annotations

import logging
from typing import Iterator, Sequence

import dask
import dask.array as dask_array
import numpy as np
import rioxarray

from .raster import SourceInfo, fill_value, is_valid_mask, read_window

logger = logging.getLogger(__name__)

Block = tuple[int, int, np.ndarray]


def average_block(
    total: np.ndarray, count: np.ndarray, fill: float
) -> np.ndarray:
    """Per-pixel ``total / count``; pixels with no valid sample get ``fill``."""
    average = np.full(total.shape, fill, dtype=np.float32)
    valid = count > 0
    if np.any(valid):
        average[valid] = (total[valid] / count[valid]).astype(np.float32)
    return average


def iter_blocks_numpy(
    sources: Sequence[str], info: SourceInfo, block_rows: int
) -> Iterator[Block]:
    """Sequential reference engine: mirrors the C++ block loop."""
    fill = fill_value(info)

    for y_offset in range(0, info.height, block_rows):
        rows = min(block_rows, info.height - y_offset)
        total = np.zeros((rows, info.width), dtype=np.float64)
        count = np.zeros((rows, info.width), dtype=np.uint32)

        for path in sources:
            data = read_window(path, 0, y_offset, info.width, rows).astype(
                np.float64
            )
            valid = is_valid_mask(data, info.nodata)
            if valid.any():
                total[valid] += data[valid]
                count[valid] += 1

        yield y_offset, rows, average_block(total, count, fill)


def _valid_mask_dask(data: dask_array.Array, nodata: float | None) -> dask_array.Array:
    valid = dask_array.isfinite(data)
    if nodata is not None:
        valid = valid & (data != np.float32(nodata))
        valid = valid & (data != nodata)
    return valid


def iter_blocks_dask(
    sources: Sequence[str],
    info: SourceInfo,
    block_rows: int,
    threads: int,
) -> Iterator[Block]:
    """Parallel engine: lazy per-file arrays, one materialised block at a time."""
    fill = fill_value(info)
    opened: list = []
    sums: list[dask_array.Array] = []
    counts: list[dask_array.Array] = []

    try:
        for path in sources:
            band = rioxarray.open_rasterio(
                path,
                masked=False,
                mask_and_scale=False,
                chunks={"x": info.width, "y": block_rows},
                # Each dataset is read by exactly one task per block compute
                # (one chunk per file, blocks are computed one after another),
                # so the per-dataset serialisation is unnecessary.
                lock=False,
            )
            opened.append(band)
            data = band.isel(band=0, drop=True).data

            valid = _valid_mask_dask(data, info.nodata)
            sums.append(dask_array.where(valid, data, 0.0).astype(np.float64))
            counts.append(valid.astype(np.uint32))

        total = dask_array.stack(sums, axis=0).sum(axis=0)
        count = dask_array.stack(counts, axis=0).sum(axis=0)

        for y_offset in range(0, info.height, block_rows):
            rows = min(block_rows, info.height - y_offset)
            total_block, count_block = dask.compute(
                total[y_offset : y_offset + rows],
                count[y_offset : y_offset + rows],
                scheduler="threads",
                num_workers=threads,
            )
            yield (
                y_offset,
                rows,
                average_block(
                    np.asarray(total_block, dtype=np.float64),
                    np.asarray(count_block, dtype=np.uint32),
                    fill,
                ),
            )
    finally:
        for band in opened:
            band.close()
