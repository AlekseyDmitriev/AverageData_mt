"""Algorithm tests, mirroring the CTest suite of the C++ implementation.

Every test runs once per engine ("dask" and "numpy"), so both code paths are
verified against the same expectations.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from average_data_mt import AverageError, AverageOptions, ENGINES, average

from _rasters import CRS, NODATA, TRANSFORM, constant, read_raster, write_raster

pytestmark = pytest.mark.parametrize("engine", ENGINES)


def options(engine: str, threads: int = 4, block_rows: int = 0) -> AverageOptions:
    return AverageOptions(threads=threads, block_rows=block_rows, engine=engine)


# ---------------------------------------------------------------------------
# 1. Basic average, output format, copied georeferencing
# ---------------------------------------------------------------------------
def test_basic_average(tmp_path, engine):
    width, height = 4, 3
    files = [
        constant(tmp_path / "a.tif", width, height, 10.0, nodata=NODATA),
        constant(tmp_path / "b.tif", width, height, 20.0, nodata=NODATA),
        constant(tmp_path / "c.tif", width, height, 30.0, nodata=NODATA),
    ]
    dst = tmp_path / "out.tif"

    stats = average(files, str(dst), options(engine, threads=3))

    assert stats.files_total == 3
    assert stats.files_used == 3
    assert stats.failed_reads == 0
    assert stats.dimension_mismatches == 0
    assert stats.blocks == 1
    assert stats.engine == engine

    out = read_raster(dst)
    assert (out.width, out.height) == (width, height)
    assert out.dtype == "float32"
    np.testing.assert_allclose(out.values, 20.0, atol=1e-6)
    assert out.compression == "LZW"

    assert out.has_nodata
    assert out.nodata == NODATA
    assert out.transform == TRANSFORM
    assert out.crs.to_string() == CRS


# ---------------------------------------------------------------------------
# 2. NoData samples are excluded, the divisor is the per-pixel valid count
# ---------------------------------------------------------------------------
def test_nodata_is_excluded(tmp_path, engine):
    a = np.full((2, 3), 10.0)
    a[0, 0] = NODATA
    a[1, 2] = NODATA
    b = np.full((2, 3), 20.0)
    b[0, 0] = NODATA
    c = np.full((2, 3), 30.0)

    files = [
        write_raster(tmp_path / "a.tif", a, nodata=NODATA),
        write_raster(tmp_path / "b.tif", b, nodata=NODATA),
        write_raster(tmp_path / "c.tif", c, nodata=NODATA),
    ]

    dst = tmp_path / "out.tif"
    average(files, str(dst), options(engine, threads=2))
    out = read_raster(dst)

    # (0,0): 10 and 20 are NoData -> only 30 remains.
    assert out.pixel(0, 0) == pytest.approx(30.0)
    # (2,1): only file a is NoData -> (20 + 30) / 2.
    assert out.pixel(2, 1) == pytest.approx(25.0)
    # (1,0) and (1,1): all three samples are valid -> 20.
    assert out.pixel(1, 0) == pytest.approx(20.0)
    assert out.pixel(1, 1) == pytest.approx(20.0)


# ---------------------------------------------------------------------------
# 3. Integer sources still produce a Float32 result (no truncation)
# ---------------------------------------------------------------------------
def test_int16_sources_are_not_truncated(tmp_path, engine):
    files = [
        constant(tmp_path / "a.tif", 2, 1, 1.0, dtype="int16"),
        constant(tmp_path / "b.tif", 2, 1, 2.0, dtype="int16"),
    ]

    dst = tmp_path / "out.tif"
    average(files, str(dst), options(engine, threads=2))

    out = read_raster(dst)
    assert out.dtype == "float32"
    assert out.pixel(0, 0) == pytest.approx(1.5)
    assert out.pixel(1, 0) == pytest.approx(1.5)


# ---------------------------------------------------------------------------
# 4. NaN is an invalid sample; pixels without samples become NoData
# ---------------------------------------------------------------------------
def test_nan_is_excluded(tmp_path, engine):
    nan = float("nan")
    a = np.array([[10.0, nan], [nan, 10.0]])
    b = np.array([[20.0, 20.0], [nan, nan]])

    files = [
        write_raster(tmp_path / "a.tif", a),
        write_raster(tmp_path / "b.tif", b),
    ]

    dst = tmp_path / "out.tif"
    average(files, str(dst), options(engine, threads=2))

    out = read_raster(dst)
    assert out.pixel(0, 0) == pytest.approx(15.0)  # 10 and 20
    assert out.pixel(1, 0) == pytest.approx(20.0)  # NaN and 20
    assert math.isnan(out.pixel(0, 1))  # NaN and NaN -> no data
    assert out.pixel(1, 1) == pytest.approx(10.0)  # 10 and NaN

    assert out.has_nodata
    assert math.isnan(out.nodata)


# ---------------------------------------------------------------------------
# 5. Accumulation is done in double precision
# ---------------------------------------------------------------------------
def test_double_precision_accumulation(tmp_path, engine):
    file_count = 100
    files = [constant(tmp_path / "big.tif", 1, 1, 1e8)]
    files += [
        constant(tmp_path / f"small_{i}.tif", 1, 1, 1.0)
        for i in range(1, file_count)
    ]

    dst = tmp_path / "out.tif"
    # A single thread keeps the sequential reference deterministic.
    average(files, str(dst), options(engine, threads=1))

    out = read_raster(dst)
    exact = (1e8 + (file_count - 1)) / file_count
    stored = float(np.float32(exact))

    # float32 accumulation would lose all 99 small values (ulp of 1e8 is 8).
    assert out.pixel(0, 0) == pytest.approx(exact, abs=0.05)
    assert out.pixel(0, 0) == pytest.approx(stored, abs=1e-3)


# ---------------------------------------------------------------------------
# 6. Same result regardless of the block height (incl. block boundaries)
# ---------------------------------------------------------------------------
@pytest.mark.parametrize(
    ("block_rows", "expected_blocks"), [(1, 5), (2, 3), (4, 2), (5, 1)]
)
def test_block_boundaries(tmp_path, engine, block_rows, expected_blocks):
    width, height = 2, 5

    a = np.full((height, width), 10.0)
    a[3, 1] = NODATA
    b = np.full((height, width), 20.0)
    b[3, 1] = NODATA
    c = np.full((height, width), 30.0)

    files = [
        write_raster(tmp_path / "a.tif", a, nodata=NODATA),
        write_raster(tmp_path / "b.tif", b, nodata=NODATA),
        write_raster(tmp_path / "c.tif", c, nodata=NODATA),
    ]

    dst = tmp_path / f"out_{block_rows}.tif"
    stats = average(
        files, str(dst), options(engine, threads=2, block_rows=block_rows)
    )
    assert stats.blocks == expected_blocks

    out = read_raster(dst)
    assert (out.width, out.height) == (width, height)
    for y in range(height):
        for x in range(width):
            hole = (x == 1 and y == 3)
            assert out.pixel(x, y) == pytest.approx(30.0 if hole else 20.0)


# ---------------------------------------------------------------------------
# 7. Unreadable and mismatched files are skipped, the rest is still averaged
# ---------------------------------------------------------------------------
def test_bad_inputs_are_skipped(tmp_path, engine):
    good_a = constant(tmp_path / "good_a.tif", 3, 2, 10.0)
    good_b = constant(tmp_path / "good_b.tif", 3, 2, 20.0)
    wrong_size = constant(tmp_path / "wrong_size.tif", 4, 2, 1000.0)
    missing = str(tmp_path / "does_not_exist.tif")

    dst = tmp_path / "out.tif"
    stats = average(
        [good_a, missing, wrong_size, good_b], str(dst), options(engine, threads=2)
    )

    assert stats.files_total == 4
    assert stats.files_used == 2
    assert stats.failed_reads == 1
    assert stats.dimension_mismatches == 1

    out = read_raster(dst)
    np.testing.assert_allclose(out.values, 15.0, atol=1e-6)


# ---------------------------------------------------------------------------
# 8. Failing runs produce no output file
# ---------------------------------------------------------------------------
def test_failures_leave_no_output(tmp_path, engine):
    dst_all_bad = tmp_path / "out_all_bad.tif"
    with pytest.raises(AverageError):
        average(
            [str(tmp_path / "missing1.tif"), str(tmp_path / "missing2.tif")],
            str(dst_all_bad),
            options(engine, threads=2),
        )
    assert not dst_all_bad.exists()

    dst_empty = tmp_path / "out_empty.tif"
    with pytest.raises(AverageError):
        average([], str(dst_empty), options(engine, threads=2))
    assert not dst_empty.exists()


# ---------------------------------------------------------------------------
# 9. Thread count is clamped to sane values
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("threads", [0, -7, 1, 64])
def test_thread_count_is_clamped(tmp_path, engine, threads):
    files = [
        constant(tmp_path / "a.tif", 2, 2, 10.0),
        constant(tmp_path / "b.tif", 2, 2, 20.0),
    ]

    dst = tmp_path / f"out_{threads}.tif"
    stats = average(files, str(dst), options(engine, threads=threads))

    assert stats.files_used == 2
    out = read_raster(dst)
    np.testing.assert_allclose(out.values, 15.0, atol=1e-6)


# ---------------------------------------------------------------------------
# 10. Single file input (average equals the input)
# ---------------------------------------------------------------------------
def test_single_file(tmp_path, engine):
    source = write_raster(tmp_path / "only.tif", [[7.5], [8.5]])

    dst = tmp_path / "out.tif"
    average([source], str(dst), options(engine, threads=8))

    out = read_raster(dst)
    assert out.pixel(0, 0) == pytest.approx(7.5)
    assert out.pixel(0, 1) == pytest.approx(8.5)
