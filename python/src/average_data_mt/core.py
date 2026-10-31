"""Averaging core: validation, block loop, output writing, statistics."""

from __future__ import annotations

import logging
import os
import sys
from pathlib import Path
from typing import Sequence

from rasterio.windows import Window

from .engines import iter_blocks_dask, iter_blocks_numpy
from .model import ENGINES, AverageError, AverageOptions, AverageStats
from .raster import (
    block_count,
    inspect_source,
    open_output,
    resolve_block_rows,
    validate_sources,
)

logger = logging.getLogger(__name__)


def average(
    files: Sequence[str],
    dst: str,
    options: AverageOptions | None = None,
) -> AverageStats:
    """Averages ``files`` pixel by pixel and writes ``dst`` as Float32 GeoTIFF.

    Raises :class:`AverageError` on failure; a partially written output file is
    removed in that case.
    """
    options = options or AverageOptions()
    files = [os.fspath(path) for path in files]

    if options.engine not in ENGINES:
        raise AverageError(
            f"unknown engine {options.engine!r}, expected one of {ENGINES}"
        )

    stats = AverageStats(files_total=len(files), engine=options.engine)
    if not files:
        raise AverageError("no input files are given")

    info = inspect_source(files[0])
    threads = max(1, min(int(options.threads), len(files)))
    block_rows = resolve_block_rows(
        int(options.block_rows),
        info.width,
        info.height,
        threads,
        int(options.memory_budget_bytes),
    )
    stats.blocks = block_count(info.height, block_rows)

    logger.info("source is %dx%d, NoData=%s", info.width, info.height, info.nodata)
    logger.info(
        "processing %d file(s) with %d thread(s) in %d block(s) of up to %d row(s)",
        len(files),
        threads,
        stats.blocks,
        block_rows,
    )

    sources = validate_sources(files, info, stats)
    stats.files_used = len(sources)
    if not sources:
        raise AverageError("none of the input files could be read")

    if stats.failed_reads or stats.dimension_mismatches:
        logger.warning(
            "average is computed over a subset of the input: %d of %d file(s) "
            "are usable",
            stats.files_used,
            stats.files_total,
        )

    if options.engine == "dask":
        blocks = iter_blocks_dask(sources, info, block_rows, threads)
    else:
        blocks = iter_blocks_numpy(sources, info, block_rows)

    try:
        with open_output(dst, info) as out:
            for y_offset, rows, values in blocks:
                out.write(
                    values, 1, window=Window(0, y_offset, info.width, rows)
                )
                if options.progress:
                    _report_progress(y_offset + rows, info.height)
    except BaseException:
        # A partially written raster must not be left behind (same as C++).
        _unlink(dst)
        raise
    finally:
        if options.progress:
            print(file=sys.stderr, flush=True)

    logger.info(
        "done: %d of %d file(s) averaged into %s",
        stats.files_used,
        stats.files_total,
        dst,
    )
    return stats


def _report_progress(done_rows: int, total_rows: int) -> None:
    percent = 100.0 * done_rows / max(1, total_rows)
    print(
        f"\r  {percent:5.1f}%  {done_rows}/{total_rows} rows",
        end="",
        file=sys.stderr,
        flush=True,
    )


def _unlink(path: str) -> None:
    try:
        Path(path).unlink()
    except OSError:
        pass
