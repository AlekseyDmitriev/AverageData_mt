"""Metadata propagation, engine equivalence and library interoperability."""

from __future__ import annotations

import numpy as np
import pytest
import rioxarray
from rasterio.enums import ColorInterp

from average_data_mt import AverageOptions, average

from _rasters import CRS, NODATA, TRANSFORM, constant, read_raster, write_raster


def test_metadata_is_copied(tmp_path):
    source = write_raster(
        tmp_path / "a.tif",
        np.full((2, 2), 5.0),
        nodata=NODATA,
        tags={"source": "unit-test", "AREA_OR_POINT": "Area"},
        band_tags={"note": "band-level"},
        unit="m",
        scale=0.001,
        offset=-1.0,
        colorinterp=ColorInterp.gray,
    )

    dst = tmp_path / "out.tif"
    average([source], str(dst), AverageOptions(threads=1))

    out = read_raster(dst)
    assert out.tags.get("source") == "unit-test"
    assert out.band_tags.get("note") == "band-level"
    assert out.unit == "m"
    assert out.scale == pytest.approx(0.001)
    assert out.offset == pytest.approx(-1.0)
    assert out.colorinterp == ColorInterp.gray
    assert out.crs.to_string() == CRS
    assert out.transform == TRANSFORM


def test_output_opens_with_rioxarray(tmp_path):
    files = [
        constant(tmp_path / "a.tif", 3, 2, 10.0, nodata=NODATA),
        constant(tmp_path / "b.tif", 3, 2, 30.0, nodata=NODATA),
    ]
    dst = tmp_path / "out.tif"
    average(files, str(dst), AverageOptions(threads=2, engine="dask"))

    with rioxarray.open_rasterio(dst) as data:
        assert data.rio.crs.to_string() == CRS
        assert data.rio.nodata == NODATA
        assert data.dtype == np.float32
        assert data.rio.transform() == TRANSFORM
        assert data.shape == (1, 2, 3)
        assert float(data.mean()) == pytest.approx(20.0)


def test_dask_and_numpy_engines_agree(tmp_path):
    rng = np.random.default_rng(20261030)
    height, width, file_count = 16, 16, 5

    files = []
    for index in range(file_count):
        values = rng.normal(100.0, 50.0, size=(height, width))
        # Sprinkle NoData holes so the per-pixel divisor differs.
        holes = rng.random((height, width)) < 0.2
        values[holes] = NODATA
        files.append(write_raster(tmp_path / f"f{index}.tif", values, nodata=NODATA))

    dask_dst = tmp_path / "dask.tif"
    numpy_dst = tmp_path / "numpy.tif"
    average(files, str(dask_dst), AverageOptions(threads=4, engine="dask"))
    average(files, str(numpy_dst), AverageOptions(threads=4, engine="numpy"))

    dask_out = read_raster(dask_dst)
    numpy_out = read_raster(numpy_dst)
    np.testing.assert_allclose(
        dask_out.values, numpy_out.values, rtol=1e-6, atol=1e-5, equal_nan=True
    )


def test_dask_agrees_with_float64_reference(tmp_path):
    rng = np.random.default_rng(7)
    height, width = 8, 8
    values_per_file = [rng.normal(0.0, 1.0, size=(height, width)) for _ in range(3)]
    values_per_file[0][0, 0] = NODATA
    values_per_file[1][3, 4] = np.nan

    files = [
        write_raster(tmp_path / f"f{index}.tif", values, nodata=NODATA)
        for index, values in enumerate(values_per_file)
    ]

    dst = tmp_path / "out.tif"
    average(files, str(dst), AverageOptions(threads=3, engine="dask", block_rows=3))

    # Reference: plain float64 numpy over the same values.
    raw = np.stack([rasterio_read(path) for path in files]).astype(np.float64)
    valid = np.isfinite(raw) & (raw != NODATA)
    total = np.where(valid, raw, 0.0).sum(axis=0)
    count = valid.sum(axis=0)
    expected = np.where(
        count > 0, total / np.maximum(count, 1), NODATA
    ).astype(np.float32)

    out = read_raster(dst)
    np.testing.assert_allclose(out.values, expected, rtol=1e-6, atol=1e-6)


def rasterio_read(path) -> np.ndarray:
    import rasterio

    with rasterio.open(path) as ds:
        return ds.read(1)
