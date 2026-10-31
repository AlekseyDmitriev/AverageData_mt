"""Per-pixel averaging of a stack of single-band GeoTIFFs.

Python port of the C++ ``AverageData_mt`` utility: NoData/NaN aware,
float64 accumulation, Float32 GeoTIFF output, block processing with bounded
memory and dask-based parallel reads.
"""

from __future__ import annotations

from .core import average
from .model import (
    DEFAULT_MEMORY_BUDGET_BYTES,
    ENGINES,
    AverageError,
    AverageOptions,
    AverageStats,
)

__all__ = [
    "DEFAULT_MEMORY_BUDGET_BYTES",
    "ENGINES",
    "AverageError",
    "AverageOptions",
    "AverageStats",
    "average",
]

__version__ = "0.1.0"
