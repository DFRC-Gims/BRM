"""Bounded-memory verification against an object's last pre-deletion bytes."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
from typing import Protocol


class Recipe(Protocol):
    size: int

    def read(self, offset: int, length: int) -> bytes:
        """Return exactly the requested bytes from the reference object."""
        ...


def count_matching_bytes(a: bytes, b: bytes) -> int:
    """Count equal bytes at equal offsets, without a Python loop per byte.

    Shifts fold each XOR byte's eight bits into that byte's least significant
    bit. The mask discards the other bits before counting unequal bytes.
    Folding shifts by at most seven bits, so adjacent bytes cannot contaminate
    the retained bits. All buffers and integers use O(len(a)) space.
    """
    size = len(a)
    if len(b) != size:
        raise ValueError("Byte comparison requires equal lengths")
    if a == b:
        return size
    zeros_a = a.count(0)
    if zeros_a == size:
        return b.count(0)
    if b.count(0) == size:
        return zeros_a
    different = int.from_bytes(a, "little") ^ int.from_bytes(b, "little")
    different |= different >> 4
    different |= different >> 2
    different |= different >> 1
    low_bits = int.from_bytes(b"\x01" * size, "little")
    return size - (different & low_bits).bit_count()


def _matching_run(a: bytes, b: bytes, minimum: int, carry: int) -> tuple[bool, int]:
    """Find a coherent equal span and retain the trailing span for the next read."""
    size = len(a)
    if a == b:
        trailing = carry + size
        return trailing >= minimum, trailing
    different = int.from_bytes(a, "little") ^ int.from_bytes(b, "little")
    different |= different >> 4
    different |= different >> 2
    different |= different >> 1
    low_bits = int.from_bytes(b"\x01" * size, "little")
    unequal = different & low_bits
    # a != b guarantees at least one set bit in unequal.
    leading = ((unequal & -unequal).bit_length() - 1) // 8
    trailing = size - 1 - (unequal.bit_length() - 1) // 8
    if carry + leading >= minimum:
        return True, trailing
    runs = low_bits ^ unequal
    span = 1
    # Each intersection doubles the represented span (or adds its remaining
    # length). Only low bits are retained, so shifting by whole bytes suffices.
    while runs and span < minimum:
        shift = min(span, minimum - span)
        runs &= runs >> (8 * shift)
        span += shift
    return bool(runs), trailing


class _ContentEvidence:
    """Track final-pattern spans without merging runs across recipe segments."""

    def __init__(self, recipe: Recipe):
        segments = getattr(recipe, "segments", None)
        if segments is None:
            self.spans = ((0, recipe.size, min(32, recipe.size)),) if recipe.size else ()
        else:
            self.spans = tuple((s.start, s.end, len(s.pattern)) for s in segments)
            end = 0
            for start, stop, minimum in self.spans:
                if start != end or stop <= start or minimum <= 0:
                    raise ValueError("Reference segments must partition the file with nonempty patterns")
                end = stop
            if end != recipe.size:
                raise ValueError("Reference segment lengths do not match the reference size")
        self.index = 0
        self.trailing = 0
        self.found = False

    def observe(self, offset: int, reference: bytes, actual: bytes) -> None:
        if self.found:
            return
        stop = offset + len(reference)
        position = offset
        while position < stop and self.index < len(self.spans):
            start, end, minimum = self.spans[self.index]
            if position >= end:
                self.index += 1
                self.trailing = 0
                continue
            finish = min(stop, end)
            left, right = position - offset, finish - offset
            # recipe.read already applies each segment's origin/phase. Exact
            # offset comparison therefore requires the same final pattern phase.
            self.found, self.trailing = _matching_run(
                reference[left:right], actual[left:right], minimum, self.trailing
            )
            if self.found:
                return
            position = finish

    def is_short_prefix(self, recovered_size: int) -> bool:
        return bool(
            self.spans
            and 0 < recovered_size < self.spans[0][2]
            and recovered_size <= self.spans[0][1]
        )


def _signature(info: os.stat_result) -> tuple[int, ...]:
    """Track size and file identity; timestamps do not establish content recovery."""
    return (
        info.st_size,
        info.st_dev,
        info.st_ino,
    )


def measure_file(
    path: Path,
    recipe: Recipe,
    expected_sha256: str,
    *,
    chunk_size: int = 1024 * 1024,
) -> dict:
    """Compare a candidate with the final reference at identical logical offsets.

    Missing bytes stay in the denominator. Extra bytes are hashed and reported
    but cannot increase the recovery rate. An empty reference has rate 1.0 only
    for an empty candidate. ``zero_filled`` describes a nonempty candidate whose
    entire recovered content is zero; reference zeros remain valid matches.

    ``has_content_evidence`` requires a coherent same-offset span at least one
    final recipe pattern long, entirely within that segment. Runs may cross read
    boundaries but not segment boundaries. Recipes without segment metadata use
    a span of min(32, expected_size). An exact short truncated prefix also counts;
    complete candidates always count. This flag does not change matched_bytes.

    A wrong reconstructed reference raises ValueError. Candidate read errors or
    changes in size or identity during measurement return an unverified record,
    with unavailable metrics set to None and a populated ``read_error``.
    Timestamps do not affect the content verdict.
    """
    if not isinstance(chunk_size, int) or isinstance(chunk_size, bool) or chunk_size <= 0:
        raise ValueError("chunk_size must be a positive integer")
    expected_size = recipe.size
    if not isinstance(expected_size, int) or isinstance(expected_size, bool) or expected_size < 0:
        raise ValueError("Recipe size must be a nonnegative integer")
    expected_sha256 = expected_sha256.lower()
    if len(expected_sha256) != 64 or any(c not in "0123456789abcdef" for c in expected_sha256):
        raise ValueError("expected_sha256 must be a hexadecimal SHA-256 digest")

    result = {
        "expected_size": expected_size,
        "recovered_size": None,
        "matched_bytes": None,
        "mismatched_bytes": None,
        "missing_bytes": None,
        "extra_bytes": None,
        "recovered_sha256": None,
        "expected_sha256": expected_sha256,
        "reference_validated": False,
        "zero_bytes": None,
        "zero_filled": None,
        "has_content_evidence": None,
        "complete": False,
        "byte_recovery_rate": None,
        "read_error": None,
    }
    path = Path(path)
    reference_hash = hashlib.sha256()
    recovered_hash = hashlib.sha256()
    recovered_size = matched = zeros = 0
    evidence = _ContentEvidence(recipe)
    try:
        before = path.stat()
        if not stat.S_ISREG(before.st_mode):
            raise OSError("Candidate is not a regular file")
        signature = _signature(before)
        with path.open("rb") as candidate:
            if _signature(os.fstat(candidate.fileno())) != signature:
                raise OSError("Candidate changed before it could be read")
            # The initial size bounds candidate reads even if another process
            # continuously appends. The final stat then invalidates that read.
            for offset in range(0, max(expected_size, before.st_size), chunk_size):
                reference_length = min(chunk_size, max(expected_size - offset, 0))
                if reference_length:
                    try:
                        reference = recipe.read(offset, reference_length)
                    except OSError as exc:
                        raise ValueError(f"Cannot reconstruct reference at offset {offset}: {exc}") from exc
                    if not isinstance(reference, bytes) or len(reference) != reference_length:
                        raise ValueError(f"Reference recipe returned invalid bytes at offset {offset}")
                    reference_hash.update(reference)
                else:
                    reference = b""

                actual_length = min(chunk_size, max(before.st_size - offset, 0))
                actual = candidate.read(actual_length) if actual_length else b""
                recovered_size += len(actual)
                recovered_hash.update(actual)
                zeros += actual.count(0)
                overlap = min(len(reference), len(actual))
                matched += count_matching_bytes(reference[:overlap], actual[:overlap])
                evidence.observe(offset, reference[:overlap], actual[:overlap])

            if reference_hash.hexdigest() != expected_sha256:
                raise ValueError("Reconstructed reference SHA-256 differs from expected_sha256")
            result["reference_validated"] = True
            if (
                recovered_size != before.st_size
                or _signature(os.fstat(candidate.fileno())) != signature
                or _signature(path.stat()) != signature
            ):
                raise OSError("Candidate changed while it was being read")
    except OSError as exc:
        result["read_error"] = f"{type(exc).__name__}: {exc}"
        return result

    recovered_sha256 = recovered_hash.hexdigest()
    complete = recovered_size == expected_size and recovered_sha256 == expected_sha256
    short_prefix = (
        recovered_size < expected_size
        and matched == recovered_size
        and evidence.is_short_prefix(recovered_size)
    )
    result.update(
        recovered_size=recovered_size,
        matched_bytes=matched,
        mismatched_bytes=min(expected_size, recovered_size) - matched,
        missing_bytes=max(expected_size - recovered_size, 0),
        extra_bytes=max(recovered_size - expected_size, 0),
        recovered_sha256=recovered_sha256,
        zero_bytes=zeros,
        zero_filled=recovered_size > 0 and zeros == recovered_size,
        has_content_evidence=complete or evidence.found or short_prefix,
        complete=complete,
        byte_recovery_rate=(matched / expected_size if expected_size else float(recovered_size == 0)),
    )
    return result
