"""Helpers for building and inspecting rasters inside tests.

Everything is created on the fly in ``tmp_path`` and removed by pytest.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import rasterio
from rasterio.enums import ColorInterp
from rasterio.transform import Affine

NODATA = -9999.0

# Something that is not EPSG:4326 on purpose, to catch a silently dropped CRS.
CRS = "EPSG:32637"
TRANSFORM = Affine(10.0, 0.0, 1000.0, 0.0, -10.0, 2000.0)


def write_raster(
    path,
    values,
    *,
    dtype: str = "float32",
    nodata: float | None = None,
    crs: str | None = CRS,
    transform: Affine = TRANSFORM,
    tags: dict[str, str] | None = None,
    band_tags: dict[str, str] | None = None,
    unit: str | None = None,
    scale: float | None = None,
    offset: float | None = None,
    colorinterp: ColorInterp | None = None,
) -> str:
    """Writes a single-band GeoTIFF; GDAL converts ``values`` to ``dtype``."""
    data = np.asarray(values, dtype=dtype)
    if data.ndim != 2:
        raise ValueError("expected a 2-D array")
    height, width = data.shape

    with rasterio.open(
        path,
        "w",
        driver="GTiff",
        width=width,
        height=height,
        count=1,
        dtype=dtype,
        crs=crs,
        transform=transform,
        nodata=nodata,
    ) as ds:
        ds.write(data, 1)
        if tags:
            ds.update_tags(**tags)
        if band_tags:
            ds.update_tags(1, **band_tags)
        if unit:
            ds.set_band_unit(1, unit)
        if scale is not None:
            ds.scales = (scale,)
        if offset is not None:
            ds.offsets = (offset,)
        if colorinterp is not None:
            ds.colorinterp = (colorinterp,)

    return str(path)


def constant(path, width: int, height: int, value: float, **kwargs) -> str:
    """A raster filled with one value."""
    values = np.full((height, width), value, dtype=np.float64)
    return write_raster(path, values, **kwargs)


@dataclass
class Raster:
    values: np.ndarray
    width: int
    height: int
    dtype: str
    nodata: float | None
    transform: Affine
    crs: object
    tags: dict = field(default_factory=dict)
    band_tags: dict = field(default_factory=dict)
    unit: str | None = None
    scale: float = 1.0
    offset: float = 0.0
    colorinterp: ColorInterp | None = None
    compression: str | None = None

    @property
    def has_nodata(self) -> bool:
        return self.nodata is not None

    def pixel(self, x: int, y: int) -> float:
        """Safe accessor: out-of-range reads return NaN instead of raising."""
        if not (0 <= x < self.width and 0 <= y < self.height):
            return float("nan")
        return float(self.values[y, x])


def read_raster(path) -> Raster:
    with rasterio.open(path) as ds:
        values = ds.read(1)
        return Raster(
            values=values,
            width=ds.width,
            height=ds.height,
            dtype=ds.dtypes[0],
            nodata=ds.nodata,
            transform=ds.transform,
            crs=ds.crs,
            tags=dict(ds.tags()),
            band_tags=dict(ds.tags(1)),
            unit=ds.units[0] or None,
            scale=float(ds.scales[0]),
            offset=float(ds.offsets[0]),
            colorinterp=ds.colorinterp[0] if ds.colorinterp else None,
            compression=ds.compression.name.upper() if ds.compression else None,
        )
