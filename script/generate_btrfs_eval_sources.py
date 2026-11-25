#!/usr/bin/env python3
"""Generate deterministic source files for Btrfs recovery evaluation v2."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Mapping

PROGRAM_VERSION = "2.0.1"
SCHEMA_VERSION = "2.0"
MARKER_NAME = ".btrfs_eval_source_set"
MANIFEST_NAME = "source_manifest.jsonl"
CONFIG_NAME = "source_config.json"
MAX_FILE_SIZE_BYTES = 1_200_000_000
SIZE_CLASSES = ("inline", "under_chunk", "over_chunk")
GENERAL_SCENARIOS = ("create", "modify", "rename", "move", "overwrite", "chain1")
SINGLE_SOURCE_SCENARIOS = ("create", "modify", "rename", "move", "chain1")
CHAIN2_TRANSITIONS: dict[str, tuple[str, str]] = {
    "under_to_inline": ("under_chunk", "inline"),
    "inline_to_under": ("inline", "under_chunk"),
    "over_to_under": ("over_chunk", "under_chunk"),
    "under_to_over": ("under_chunk", "over_chunk"),
}
DEFAULT_CHAIN2_COUNTS = {transition: 50 for transition in CHAIN2_TRANSITIONS}
DEFAULT_SIZE_RANGES: dict[str, tuple[int, int]] = {
    "inline": (256, 1536),
    "under_chunk": (64 * 1024, 16 * 1024 * 1024),
    "over_chunk": (32 * 1024 * 1024, 1024 * 1024 * 1024),
}
SIZE_ALIGNMENTS = {
    "inline": 1,
    "under_chunk": 4096,
    "over_chunk": 1024 * 1024,
}
SIZE_DISTRIBUTIONS = {
    "inline": "linear_stratified",
    "under_chunk": "logarithmic_stratified",
    "over_chunk": "logarithmic_stratified",
}
_SIZE_RE = re.compile(r"^(?P<number>[0-9]+)(?P<unit>K|KiB|M|MiB|G|GiB)?$", re.I)
_UNIT_MULTIPLIERS = {
    "": 1,
    "k": 1024,
    "kib": 1024,
    "m": 1024**2,
    "mib": 1024**2,
    "g": 1024**3,
    "gib": 1024**3,
}


class GenerationError(RuntimeError):
    """Dataset generation could not be completed."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace(
        "+00:00", "Z"
    )


def parse_size(text: str) -> int:
    match = _SIZE_RE.fullmatch(text.strip())
    if not match:
        raise argparse.ArgumentTypeError(
            f"invalid size {text!r}; use bytes or K/KiB, M/MiB, G/GiB"
        )
    number = int(match.group("number"))
    if number <= 0:
        raise argparse.ArgumentTypeError("size must be greater than zero")
    return number * _UNIT_MULTIPLIERS[(match.group("unit") or "").lower()]


def human_bytes(value: int) -> str:
    for suffix, divisor in (("GiB", 1024**3), ("MiB", 1024**2), ("KiB", 1024)):
        if value >= divisor:
            return f"{value / divisor:.2f} {suffix}"
    return f"{value} B"


def write_json(path: Path, payload: Any) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")


def write_jsonl(path: Path, rows: Iterable[Mapping[str, Any]]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(dict(row), ensure_ascii=False, sort_keys=True))
            handle.write("\n")


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def _stable_fraction(label: str) -> float:
    digest = hashlib.sha256(label.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big") / float(1 << 64)


def _aligned_bounds(minimum: int, maximum: int, alignment: int) -> tuple[int, int]:
    aligned_min = ((minimum + alignment - 1) // alignment) * alignment
    aligned_max = (maximum // alignment) * alignment
    if aligned_min > aligned_max:
        raise ValueError(
            f"range {minimum}..{maximum} cannot produce a value aligned to {alignment}"
        )
    return aligned_min, aligned_max


def validate_size_policy(
    sizes: Mapping[str, int] | None,
    size_ranges: Mapping[str, tuple[int, int]] | None,
) -> str:
    if (sizes is None) == (size_ranges is None):
        raise ValueError("provide exactly one of sizes or size_ranges")
    mapping: Mapping[str, Any] = sizes if sizes is not None else size_ranges or {}
    if set(mapping) != set(SIZE_CLASSES):
        raise ValueError(f"size policy must contain exactly: {', '.join(SIZE_CLASSES)}")

    bounds: list[tuple[int, int]] = []
    for size_class in SIZE_CLASSES:
        if sizes is not None:
            value = int(sizes[size_class])
            minimum = maximum = value
        else:
            raw = size_ranges[size_class]  # type: ignore[index]
            if len(raw) != 2:
                raise ValueError(f"range for {size_class} must contain two integers")
            minimum, maximum = int(raw[0]), int(raw[1])
        if minimum <= 0 or maximum <= 0 or minimum > maximum:
            raise ValueError(f"invalid size range for {size_class}: {minimum}..{maximum}")
        if maximum >= MAX_FILE_SIZE_BYTES:
            raise ValueError("each file size must be below 1,200,000,000 bytes")
        if sizes is None:
            _aligned_bounds(minimum, maximum, SIZE_ALIGNMENTS[size_class])
        bounds.append((minimum, maximum))

    if not (bounds[0][1] < bounds[1][0] and bounds[1][1] < bounds[2][0]):
        raise ValueError(
            "size classes must be strictly ordered and non-overlapping: "
            "inline < under_chunk < over_chunk"
        )
    return "fixed" if sizes is not None else "varied"


def _select_size(
    *,
    size_class: str,
    case_index: int,
    bucket_count: int,
    identity: str,
    seed: int,
    sizes: Mapping[str, int] | None,
    size_ranges: Mapping[str, tuple[int, int]] | None,
) -> tuple[int, dict[str, Any]]:
    if sizes is not None:
        value = int(sizes[size_class])
        return value, {
            "size_mode": "fixed",
            "size_range_min": value,
            "size_range_max": value,
            "size_distribution": "fixed",
            "size_alignment": 1,
            "size_bucket_index": 1,
            "size_bucket_count": 1,
        }

    assert size_ranges is not None
    configured_min, configured_max = size_ranges[size_class]
    alignment = SIZE_ALIGNMENTS[size_class]
    minimum, maximum = _aligned_bounds(configured_min, configured_max, alignment)
    if bucket_count <= 0:
        raise ValueError("bucket count must be positive")
    jitter = 0.10 + 0.80 * _stable_fraction(f"{seed}|{identity}|size")
    fraction = ((case_index - 1) + jitter) / bucket_count
    distribution = SIZE_DISTRIBUTIONS[size_class]
    if minimum == maximum:
        raw = float(minimum)
    elif distribution == "linear_stratified":
        raw = minimum + (maximum - minimum) * fraction
    else:
        raw = math.exp(math.log(minimum) + (math.log(maximum) - math.log(minimum)) * fraction)
    selected = int(round(raw / alignment)) * alignment
    selected = max(minimum, min(selected, maximum))
    if selected >= MAX_FILE_SIZE_BYTES:
        raise AssertionError("selected size violates hard maximum")
    return selected, {
        "size_mode": "varied",
        "size_range_min": configured_min,
        "size_range_max": configured_max,
        "size_distribution": distribution,
        "size_alignment": alignment,
        "size_bucket_index": case_index,
        "size_bucket_count": bucket_count,
    }


def _class_id(size_class: str) -> str:
    return size_class.upper()


def _case_id(scenario: str, qualifier: str, case_index: int) -> str:
    return f"CASE-{scenario.upper()}-{qualifier.upper()}-{case_index:03d}"


def _object_id(scenario: str, role: str, qualifier: str, case_index: int) -> str:
    return f"OBJ-{scenario.upper()}-{role.upper()}-{qualifier.upper()}-{case_index:03d}"


def _source_id(scenario: str, role: str, qualifier: str, case_index: int) -> str:
    return f"SRC-{scenario.upper()}-{role.upper()}-{qualifier.upper()}-{case_index:03d}"


def _write_pattern_file(path: Path, size: int, token: str) -> str:
    pattern = (token + " ").encode("utf-8")
    if not pattern:
        raise ValueError("content pattern must not be empty")
    digest = hashlib.sha256()
    path.parent.mkdir(parents=True, exist_ok=True)
    block_repeats = max(1, (1024 * 1024) // len(pattern))
    block = pattern * block_repeats
    remaining = size
    with path.open("wb") as handle:
        while remaining >= len(block):
            handle.write(block)
            digest.update(block)
            remaining -= len(block)
        if remaining:
            tail = (pattern * ((remaining + len(pattern) - 1) // len(pattern)))[:remaining]
            handle.write(tail)
            digest.update(tail)
        handle.flush()
        os.fsync(handle.fileno())
    return digest.hexdigest()


def _validate_counts(count_per_class: int, chain2_counts: Mapping[str, int]) -> None:
    if count_per_class < 0:
        raise ValueError("count_per_class must be zero or greater")
    if set(chain2_counts) != set(CHAIN2_TRANSITIONS):
        raise ValueError(
            "chain2_counts must contain exactly: " + ", ".join(CHAIN2_TRANSITIONS)
        )
    for transition, count in chain2_counts.items():
        if not isinstance(count, int) or isinstance(count, bool) or count < 0:
            raise ValueError(f"count for {transition} must be a non-negative integer")


def plan_dataset(
    *, count_per_class: int = 100, chain2_counts: Mapping[str, int] | None = None
) -> dict[str, Any]:
    counts = dict(DEFAULT_CHAIN2_COUNTS if chain2_counts is None else chain2_counts)
    _validate_counts(count_per_class, counts)
    initial_counts = Counter({size_class: len(GENERAL_SCENARIOS) * count_per_class for size_class in SIZE_CLASSES})
    for transition, count in counts.items():
        initial_class, _ = CHAIN2_TRANSITIONS[transition]
        initial_counts[initial_class] += count
    scenario_cases = len(GENERAL_SCENARIOS) * len(SIZE_CLASSES) * count_per_class + sum(counts.values())
    source_files = (
        len(SINGLE_SOURCE_SCENARIOS) * len(SIZE_CLASSES) * count_per_class
        + 2 * len(SIZE_CLASSES) * count_per_class
        + sum(counts.values())
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "scenario_case_count": scenario_cases,
        "source_file_count": source_files,
        "initial_size_class_counts": {size_class: initial_counts[size_class] for size_class in SIZE_CLASSES},
        "chain2_counts": counts,
    }


def _base_record(
    *,
    source_entry_id: str,
    case_id: str,
    object_id: str,
    scenario_type: str,
    object_role: str,
    case_index: int,
    initial_size_class: str,
    final_size_class: str,
    transition_type: str | None,
    file_name: str,
    relative_path: str,
    content_token: str,
    size: int,
    size_meta: Mapping[str, Any],
    seed: int,
    btrfs_initial_copy: bool,
) -> dict[str, Any]:
    return {
        "source_entry_id": source_entry_id,
        "case_id": case_id,
        "object_id": object_id,
        "scenario_type": scenario_type,
        "object_role": object_role,
        "case_index": case_index,
        "initial_size_class": initial_size_class,
        "final_size_class": final_size_class,
        "transition_type": transition_type,
        "file_name": file_name,
        "relative_path": relative_path,
        "content_token": content_token,
        "content_pattern": content_token + " ",
        "actual_size": size,
        "requested_size": size,
        "hash_algorithm": "SHA-256",
        "hash_scope": "entire_file",
        "hash_encoding": "lowercase_hex",
        "mutation_seed": f"{seed}:{object_id}:mutation",
        "btrfs_initial_copy": btrfs_initial_copy,
        **dict(size_meta),
    }


def _plan_records(
    *,
    count_per_class: int,
    chain2_counts: Mapping[str, int],
    sizes: Mapping[str, int] | None,
    size_ranges: Mapping[str, tuple[int, int]] | None,
    seed: int,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []

    for scenario in SINGLE_SOURCE_SCENARIOS:
        for size_class in SIZE_CLASSES:
            for case_index in range(1, count_per_class + 1):
                qualifier = size_class
                case_id = _case_id(scenario, qualifier, case_index)
                object_id = _object_id(scenario, "subject", qualifier, case_index)
                stem = f"{scenario}_{size_class}_{case_index:03d}"
                file_name = stem + ".txt"
                size, size_meta = _select_size(
                    size_class=size_class,
                    case_index=case_index,
                    bucket_count=max(1, count_per_class),
                    identity=f"{scenario}|{size_class}|{case_index}",
                    seed=seed,
                    sizes=sizes,
                    size_ranges=size_ranges,
                )
                row = _base_record(
                    source_entry_id=_source_id(scenario, "subject", qualifier, case_index),
                    case_id=case_id,
                    object_id=object_id,
                    scenario_type=scenario,
                    object_role="subject",
                    case_index=case_index,
                    initial_size_class=size_class,
                    final_size_class=size_class,
                    transition_type=None,
                    file_name=file_name,
                    relative_path=f"objects/{scenario}/{size_class}/{file_name}",
                    content_token=stem,
                    size=size,
                    size_meta=size_meta,
                    seed=seed,
                    btrfs_initial_copy=True,
                )
                if scenario in {"modify", "chain1"}:
                    row["modified_content_token"] = f"modified_{stem}"
                rows.append(row)

    for size_class in SIZE_CLASSES:
        for case_index in range(1, count_per_class + 1):
            case_id = _case_id("overwrite", size_class, case_index)
            stem = f"overwrite_{size_class}_{case_index:03d}"
            file_name = stem + ".txt"
            incoming_id = _object_id("overwrite", "incoming", size_class, case_index)
            target_id = _object_id("overwrite", "target", size_class, case_index)
            for role, object_id, paired, initial_copy in (
                ("incoming", incoming_id, target_id, True),
                ("target", target_id, incoming_id, False),
            ):
                size, size_meta = _select_size(
                    size_class=size_class,
                    case_index=case_index,
                    bucket_count=max(1, count_per_class),
                    identity=f"overwrite|{role}|{size_class}|{case_index}",
                    seed=seed,
                    sizes=sizes,
                    size_ranges=size_ranges,
                )
                content_token = f"{role}_{stem}"
                row = _base_record(
                    source_entry_id=_source_id("overwrite", role, size_class, case_index),
                    case_id=case_id,
                    object_id=object_id,
                    scenario_type="overwrite",
                    object_role=f"overwrite_{role}",
                    case_index=case_index,
                    initial_size_class=size_class,
                    final_size_class=size_class,
                    transition_type=None,
                    file_name=file_name,
                    relative_path=f"objects/overwrite/{role}/{size_class}/{file_name}",
                    content_token=content_token,
                    size=size,
                    size_meta=size_meta,
                    seed=seed,
                    btrfs_initial_copy=initial_copy,
                )
                row["paired_object_id"] = paired
                rows.append(row)

    for transition, count in chain2_counts.items():
        initial_class, final_class = CHAIN2_TRANSITIONS[transition]
        for case_index in range(1, count + 1):
            case_id = _case_id("chain2", transition, case_index)
            original_id = _object_id("chain2", "original", transition, case_index)
            copy_id = _object_id("chain2", "copy", transition, case_index)
            stem = f"chain2_{transition}_{case_index:03d}"
            file_name = stem + ".txt"
            initial_size, size_meta = _select_size(
                size_class=initial_class,
                case_index=case_index,
                bucket_count=max(1, count),
                identity=f"chain2|{transition}|initial|{case_index}",
                seed=seed,
                sizes=sizes,
                size_ranges=size_ranges,
            )
            target_size, target_meta = _select_size(
                size_class=final_class,
                case_index=case_index,
                bucket_count=max(1, count),
                identity=f"chain2|{transition}|target|{case_index}",
                seed=seed,
                sizes=sizes,
                size_ranges=size_ranges,
            )
            row = _base_record(
                source_entry_id=_source_id("chain2", "original", transition, case_index),
                case_id=case_id,
                object_id=original_id,
                scenario_type="chain2",
                object_role="chain2_original",
                case_index=case_index,
                initial_size_class=initial_class,
                final_size_class=final_class,
                transition_type=transition,
                file_name=file_name,
                relative_path=f"objects/chain2/{transition}/{file_name}",
                content_token=stem,
                size=initial_size,
                size_meta=size_meta,
                seed=seed,
                btrfs_initial_copy=True,
            )
            row.update(
                {
                    "copy_object_id": copy_id,
                    "modified_content_token": f"modified_{stem}",
                    "resized_content_token": f"resized_{stem}",
                    "resize_target_size": target_size,
                    "resize_size_mode": target_meta["size_mode"],
                    "resize_size_range_min": target_meta["size_range_min"],
                    "resize_size_range_max": target_meta["size_range_max"],
                    "resize_size_distribution": target_meta["size_distribution"],
                    "resize_size_alignment": target_meta["size_alignment"],
                    "resize_size_bucket_index": target_meta["size_bucket_index"],
                    "resize_size_bucket_count": target_meta["size_bucket_count"],
                }
            )
            rows.append(row)

    return rows


def generate_corpus(
    *,
    output_dir: Path,
    count_per_class: int = 100,
    chain2_counts: Mapping[str, int] | None = None,
    sizes: Mapping[str, int] | None = None,
    size_ranges: Mapping[str, tuple[int, int]] | None = None,
    seed: int = 20260811,
    force: bool = False,
    skip_space_check: bool = False,
    quiet: bool = False,
) -> dict[str, Any]:
    output_dir = Path(output_dir).expanduser().resolve()
    chain2 = dict(DEFAULT_CHAIN2_COUNTS if chain2_counts is None else chain2_counts)
    _validate_counts(count_per_class, chain2)
    if sizes is None and size_ranges is None:
        size_ranges = dict(DEFAULT_SIZE_RANGES)
    size_mode = validate_size_policy(sizes, size_ranges)

    if output_dir.exists():
        if not force:
            raise GenerationError(f"output directory already exists: {output_dir}")
        shutil.rmtree(output_dir)
    output_dir.parent.mkdir(parents=True, exist_ok=True)

    rows = _plan_records(
        count_per_class=count_per_class,
        chain2_counts=chain2,
        sizes=sizes,
        size_ranges=size_ranges,
        seed=seed,
    )
    planned_bytes = sum(int(row["actual_size"]) for row in rows)
    if not skip_space_check:
        free = shutil.disk_usage(output_dir.parent).free
        required = int(planned_bytes * 1.05) + 32 * 1024 * 1024
        if free < required:
            raise GenerationError(
                f"insufficient free space: need about {human_bytes(required)}, "
                f"have {human_bytes(free)}; use --skip-space-check to bypass"
            )

    output_dir.mkdir(parents=True)
    generated_at = utc_now()
    try:
        total = len(rows)
        for index, row in enumerate(rows, 1):
            destination = output_dir / str(row["relative_path"])
            row["sha256"] = _write_pattern_file(
                destination, int(row["actual_size"]), str(row["content_token"])
            )
            row["created_at_utc"] = utc_now()
            if not quiet:
                print(f"[{index}/{total}] {row['relative_path']} ({human_bytes(int(row['actual_size']))})")

        manifest_path = output_dir / MANIFEST_NAME
        write_jsonl(manifest_path, rows)
        manifest_sha256 = sha256_file(manifest_path)
        plan = plan_dataset(count_per_class=count_per_class, chain2_counts=chain2)
        source_class_counts = Counter(str(row["initial_size_class"]) for row in rows)
        config = {
            "program_version": PROGRAM_VERSION,
            "schema_version": SCHEMA_VERSION,
            "generated_at_utc": generated_at,
            "output_dir": str(output_dir),
            "seed": seed,
            "size_classes": list(SIZE_CLASSES),
            "size_mode": size_mode,
            "size_policy": {
                size_class: (
                    {"minimum": int(sizes[size_class]), "maximum": int(sizes[size_class]), "distribution": "fixed"}
                    if sizes is not None
                    else {
                        "minimum": int(size_ranges[size_class][0]),  # type: ignore[index]
                        "maximum": int(size_ranges[size_class][1]),  # type: ignore[index]
                        "distribution": SIZE_DISTRIBUTIONS[size_class],
                    }
                )
                for size_class in SIZE_CLASSES
            },
            "count_per_class": count_per_class,
            "chain2_counts": chain2,
            "scenario_case_count": plan["scenario_case_count"],
            "source_file_count": len(rows),
            "initial_size_class_counts": plan["initial_size_class_counts"],
            "source_file_size_class_counts": {size_class: source_class_counts[size_class] for size_class in SIZE_CLASSES},
            "planned_source_bytes": planned_bytes,
            "maximum_file_size_exclusive": MAX_FILE_SIZE_BYTES,
            "source_manifest": MANIFEST_NAME,
            "source_manifest_sha256": manifest_sha256,
            "file_hash": {
                "algorithm": "SHA-256",
                "scope": "entire_file",
                "encoding": "lowercase_hex",
                "field": "sha256",
            },
            "payload_mode": "manifest_content_token_repetition",
            "overwrite_payload_tokens": {
                "incoming": "incoming_<filename-stem>",
                "target": "target_<filename-stem>",
            },
        }
        write_json(output_dir / CONFIG_NAME, config)
        (output_dir / MARKER_NAME).write_text(
            json.dumps(
                {"program_version": PROGRAM_VERSION, "schema_version": SCHEMA_VERSION},
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
    except Exception:
        shutil.rmtree(output_dir, ignore_errors=True)
        raise

    summary = {
        **plan,
        "program_version": PROGRAM_VERSION,
        "output_dir": str(output_dir),
        "planned_source_bytes": planned_bytes,
        "source_manifest_sha256": manifest_sha256,
    }
    if not quiet:
        print(json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True))
    return summary


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate the external source set for Btrfs evaluation v2."
    )
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--count-per-class", type=int, default=100)
    parser.add_argument("--chain2-under-to-inline-count", type=int, default=50)
    parser.add_argument("--chain2-inline-to-under-count", type=int, default=50)
    parser.add_argument("--chain2-over-to-under-count", type=int, default=50)
    parser.add_argument("--chain2-under-to-over-count", type=int, default=50)
    parser.add_argument("--seed", type=int, default=20260811)

    parser.add_argument("--inline-min-size", type=parse_size, default=DEFAULT_SIZE_RANGES["inline"][0])
    parser.add_argument("--inline-max-size", type=parse_size, default=DEFAULT_SIZE_RANGES["inline"][1])
    parser.add_argument("--under-min-size", type=parse_size, default=DEFAULT_SIZE_RANGES["under_chunk"][0])
    parser.add_argument("--under-max-size", type=parse_size, default=DEFAULT_SIZE_RANGES["under_chunk"][1])
    parser.add_argument("--over-min-size", type=parse_size, default=DEFAULT_SIZE_RANGES["over_chunk"][0])
    parser.add_argument("--over-max-size", type=parse_size, default=DEFAULT_SIZE_RANGES["over_chunk"][1])

    parser.add_argument("--inline-size", type=parse_size)
    parser.add_argument("--under-size", type=parse_size)
    parser.add_argument("--over-size", type=parse_size)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--skip-space-check", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--version", action="version", version=f"%(prog)s {PROGRAM_VERSION}")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    fixed_values = (args.inline_size, args.under_size, args.over_size)
    if any(value is not None for value in fixed_values) and not all(
        value is not None for value in fixed_values
    ):
        parser.error("fixed-size mode requires --inline-size, --under-size, and --over-size")
    sizes = (
        {
            "inline": int(args.inline_size),
            "under_chunk": int(args.under_size),
            "over_chunk": int(args.over_size),
        }
        if all(value is not None for value in fixed_values)
        else None
    )
    size_ranges = (
        None
        if sizes is not None
        else {
            "inline": (args.inline_min_size, args.inline_max_size),
            "under_chunk": (args.under_min_size, args.under_max_size),
            "over_chunk": (args.over_min_size, args.over_max_size),
        }
    )
    chain2_counts = {
        "under_to_inline": args.chain2_under_to_inline_count,
        "inline_to_under": args.chain2_inline_to_under_count,
        "over_to_under": args.chain2_over_to_under_count,
        "under_to_over": args.chain2_under_to_over_count,
    }
    try:
        generate_corpus(
            output_dir=args.output_dir,
            count_per_class=args.count_per_class,
            chain2_counts=chain2_counts,
            sizes=sizes,
            size_ranges=size_ranges,
            seed=args.seed,
            force=args.force,
            skip_space_check=args.skip_space_check,
            quiet=args.quiet,
        )
    except (GenerationError, ValueError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
