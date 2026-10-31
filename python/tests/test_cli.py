"""End-to-end tests of the command line interface (subprocess, like the C++ exe)."""

from __future__ import annotations

import subprocess
import sys

import pytest

from _rasters import NODATA, constant, read_raster

CLI = [sys.executable, "-m", "average_data_mt"]


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [*CLI, *args], capture_output=True, text=True, timeout=300, check=False
    )


def test_cli_averages_and_reports(tmp_path):
    files = [
        constant(tmp_path / "a.tif", 3, 2, 10.0, nodata=NODATA),
        constant(tmp_path / "b.tif", 3, 2, 20.0, nodata=NODATA),
    ]
    list_file = tmp_path / "list.txt"
    # CRLF and blank lines are tolerated, as in the C++ version.
    list_file.write_text("\r\n".join(files) + "\r\n\r\n", encoding="utf-8")
    dst = tmp_path / "out.tif"

    result = run("-input_file_list", str(list_file), "-dst", str(dst), "-threads", "2")

    assert result.returncode == 0, result.stderr
    assert "averaged 2 of 2" in result.stdout

    out = read_raster(dst)
    assert out.pixel(0, 0) == pytest.approx(15.0)


def test_cli_numpy_engine(tmp_path):
    files = [constant(tmp_path / "a.tif", 2, 2, 4.0)]
    list_file = tmp_path / "list.txt"
    list_file.write_text("\n".join(files), encoding="utf-8")
    dst = tmp_path / "out.tif"

    result = run(
        "-input_file_list",
        str(list_file),
        "-dst",
        str(dst),
        "--engine",
        "numpy",
        "--block-rows",
        "1",
    )

    assert result.returncode == 0, result.stderr
    assert read_raster(dst).pixel(1, 1) == pytest.approx(4.0)


def test_cli_missing_list_file(tmp_path):
    result = run(
        "-input_file_list", str(tmp_path / "nope.txt"), "-dst", str(tmp_path / "o.tif")
    )

    assert result.returncode != 0
    assert "cannot read" in result.stderr


def test_cli_all_inputs_bad(tmp_path):
    list_file = tmp_path / "list.txt"
    list_file.write_text(
        "\n".join([str(tmp_path / "missing1.tif"), str(tmp_path / "missing2.tif")]),
        encoding="utf-8",
    )
    dst = tmp_path / "out.tif"

    result = run("-input_file_list", str(list_file), "-dst", str(dst))

    assert result.returncode != 0
    assert not dst.exists()


def test_cli_empty_list(tmp_path):
    list_file = tmp_path / "list.txt"
    list_file.write_text("\n\n", encoding="utf-8")

    result = run(
        "-input_file_list", str(list_file), "-dst", str(tmp_path / "out.tif")
    )

    assert result.returncode != 0
    assert "no input files" in result.stderr
