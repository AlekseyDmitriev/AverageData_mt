"""Command line front-end, mirroring the C++ AverageData_mt arguments:

    average-data-mt -input_file_list <list.txt> -dst <output.tif> [-threads <n>]
"""

from __future__ import annotations

import argparse
import logging
import sys
from typing import Sequence

from .core import average
from .model import AverageError, AverageOptions

logger = logging.getLogger("average_data_mt")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="average-data-mt",
        description=(
            "Per-pixel average of a stack of single-band GeoTIFFs. NoData and "
            "NaN samples are excluded, the result is always Float32."
        ),
    )
    parser.add_argument(
        "-input_file_list",
        metavar="FILE",
        required=True,
        help="text file with one raster path per line",
    )
    parser.add_argument(
        "-dst", metavar="FILE", required=True, help="output GeoTIFF"
    )
    parser.add_argument(
        "-threads",
        type=int,
        default=4,
        metavar="N",
        help="number of worker threads (default: 4)",
    )
    parser.add_argument(
        "--engine",
        choices=("dask", "numpy"),
        default="dask",
        help="dask: parallel block reads (default), numpy: sequential",
    )
    parser.add_argument(
        "--block-rows",
        type=int,
        default=0,
        metavar="N",
        help="rows per block (default: chosen from the memory budget)",
    )
    parser.add_argument(
        "--progress", action="store_true", help="print block progress to stderr"
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="count",
        default=0,
        help="more log output (repeat for debug)",
    )
    return parser


def read_file_list(path: str) -> list[str]:
    """Reads a list of paths: one per line, CRLF and empty lines tolerated."""
    with open(path, encoding="utf-8-sig") as handle:
        return [line.strip() for line in handle if line.strip()]


def configure_logging(verbosity: int) -> None:
    level = logging.WARNING
    if verbosity == 1:
        level = logging.INFO
    elif verbosity >= 2:
        level = logging.DEBUG
    logging.basicConfig(level=level, format="%(levelname)s: %(message)s")


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    configure_logging(args.verbose)

    try:
        files = read_file_list(args.input_file_list)
    except OSError as exc:
        print(f"error: cannot read {args.input_file_list}: {exc}", file=sys.stderr)
        return 2

    if not files:
        print(f"error: no input files found in {args.input_file_list}", file=sys.stderr)
        return 1

    options = AverageOptions(
        threads=args.threads,
        block_rows=args.block_rows,
        engine=args.engine,
        progress=args.progress,
    )

    try:
        stats = average(files, args.dst, options)
    except AverageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(
        f"averaged {stats.files_used} of {stats.files_total} file(s) "
        f"in {stats.blocks} block(s) -> {args.dst}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
