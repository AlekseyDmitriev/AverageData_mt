"""Raster input/output: metadata, validation, windowed reads, output creation.

rioxarray is used for opening rasters as labelled arrays, rasterio for the
low-level parts that rioxarray does not expose directly (windowed block reads,
band unit/scale/offset/color interpretation of the result).
"""

from __future__ import annotations

import logging
import os
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Iterator, Sequence

import numpy as np
import rasterio
from rasterio.crs import CRS
from rasterio.enums import ColorInterp
from rasterio.errors import RasterioError
from rasterio.transform import Affine
from rasterio.windows import Window

from .model import AverageError, AverageStats

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class SourceInfo:
    """Metadata of the first input raster, copied to the output."""

    path: str
    width: int
    height: int
    nodata: float | None
    crs: CRS | None
    transform: Affine
    tags: dict[str, str]
    band_tags: dict[str, str]
    unit: str | None
    scale: float
    offset: float
    colorinterp: ColorInterp | None
    dtype: str


def inspect_source(path: str | os.PathLike[str]) -> SourceInfo:
    """Opens ``path`` and collects everything the output has to inherit."""
    try:
        with rasterio.open(path) as ds:
            if ds.width <= 0 or ds.height <= 0 or ds.count < 1:
                raise AverageError(
                    f"{path} has no usable raster band 1 ({ds.width}x{ds.height})"
                )
            return SourceInfo(
                path=str(path),
                width=ds.width,
                height=ds.height,
                nodata=ds.nodata,
                crs=ds.crs,
                transform=ds.transform,
                tags=dict(ds.tags()),
                band_tags=dict(ds.tags(1)),
                unit=ds.units[0] or None,
                scale=float(ds.scales[0]),
                offset=float(ds.offsets[0]),
                colorinterp=ds.colorinterp[0] if ds.colorinterp else None,
                dtype=ds.dtypes[0],
            )
    except AverageError:
        raise
    except (RasterioError, OSError) as exc:
        raise AverageError(f"cannot open {path}: {exc}") from exc


def validate_sources(
    files: Sequence[str], info: SourceInfo, stats: AverageStats
) -> list[str]:
    """Returns the files that can actually take part in the average.

    Files that cannot be opened or whose size differs from the first raster are
    skipped (and counted), exactly like the C++ implementation.
    """
    usable: list[str] = []
    for path in files:
        try:
            with rasterio.open(path) as ds:
                if ds.count < 1:
                    raise AverageError(f"{path} has no raster band 1")
                if (ds.width, ds.height) != (info.width, info.height):
                    logger.error(
                        'raster dimension of "%s" does not equal the first image! '
                        "current image size is %dx%d",
                        path,
                        ds.width,
                        ds.height,
                    )
                    stats.dimension_mismatches += 1
                    continue
        except AverageError as exc:
            logger.error("%s", exc)
            stats.failed_reads += 1
            continue
        except (RasterioError, OSError) as exc:
            logger.error("error reading file %s: %s", path, exc)
            stats.failed_reads += 1
            continue
        usable.append(path)
    return usable


def resolve_block_rows(
    requested_rows: int, width: int, height: int, threads: int, budget_bytes: int
) -> int:
    """Applies one source block height (0 => automatic) to a raster width."""
    if requested_rows > 0:
        return min(requested_rows, height)

    # Sum (float64) + count (uint32) + read buffer (float32) per pixel, times
    # the number of buffers alive at the same time.
    bytes_per_row = width * (8 + 4 + 4) * (threads + 1)
    rows = max(1, budget_bytes // max(1, bytes_per_row))
    return min(rows, height)


def block_count(height: int, block_rows: int) -> int:
    return -(-height // block_rows)


def read_window(
    path: str, x0: int, y0: int, width: int, height: int
) -> np.ndarray:
    """Reads band 1 of a window without any scaling or masking applied."""
    with rasterio.open(path) as ds:
        return ds.read(1, window=Window(x0, y0, width, height))


def is_valid_mask(data: np.ndarray, nodata: float | None) -> np.ndarray:
    """A sample counts only if it is finite and not equal to the NoData value."""
    valid = np.isfinite(data)
    if nodata is not None:
        # Compare both as read and as stored in the source data type, so that
        # float32/float64 round-trips do not hide the NoData value.
        valid &= data != np.float64(np.float32(nodata))
        valid &= data != np.float64(nodata)
    return valid


def fill_value(info: SourceInfo) -> float:
    """Value written where a pixel has no valid sample at all."""
    return float(info.nodata) if info.nodata is not None else float("nan")


@contextmanager
def open_output(dst: str, info: SourceInfo) -> Iterator[rasterio.DatasetWriter]:
    """Creates the Float32 result and copies the metadata of the first input."""
    out_nodata = info.nodata if info.nodata is not None else float("nan")
    profile = {
        "driver": "GTiff",
        "width": info.width,
        "height": info.height,
        "count": 1,
        "dtype": "float32",
        "crs": info.crs,
        "transform": info.transform,
        "compress": "LZW",
        "predictor": 3,
        "nodata": out_nodata,
    }

    try:
        ds = rasterio.open(dst, "w", **profile)
    except (RasterioError, OSError) as exc:
        raise AverageError(f"cannot create {dst}: {exc}") from exc

    with ds:
        if info.tags:
            ds.update_tags(**info.tags)
        if info.band_tags:
            ds.update_tags(1, **info.band_tags)
        if info.unit:
            ds.set_band_unit(1, info.unit)
        if info.scale != 1.0:
            ds.scales = (info.scale,)
        if info.offset != 0.0:
            ds.offsets = (info.offset,)
        if info.colorinterp is not None:
            ds.colorinterp = (info.colorinterp,)
        yield ds
