"""Settings and result types shared by the averaging code."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

# Memory budget for one block: all per-thread buffers and the merged block
# buffers must fit into it. Same value as the C++ implementation.
DEFAULT_MEMORY_BUDGET_BYTES = 256 * 1024 * 1024

Engine = Literal["dask", "numpy"]
ENGINES: tuple[str, ...] = ("dask", "numpy")


@dataclass(frozen=True)
class AverageOptions:
    """Options of a single averaging run."""

    threads: int = 4
    """Worker threads, clamped to [1, number of files]."""

    block_rows: int = 0
    """Rows per block; 0 means "choose from the memory budget"."""

    engine: Engine = "dask"
    """``"dask"`` - parallel block reads through dask, ``"numpy"`` - sequential."""

    progress: bool = False
    """Print block progress to stderr."""

    memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES
    """Upper bound for the buffers of one block (block_rows = 0 only)."""


@dataclass
class AverageStats:
    """Counters collected during a run (logging, tests, CLI summary)."""

    files_total: int = 0
    files_used: int = 0
    failed_reads: int = 0
    dimension_mismatches: int = 0
    blocks: int = 0
    engine: str = ""


class AverageError(RuntimeError):
    """Raised when a run cannot produce a result (no output is left behind)."""
