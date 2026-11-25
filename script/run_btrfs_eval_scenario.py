#!/usr/bin/env python3
"""Execute the deletion-final Btrfs evaluation scenario and write schema-2.0 ground truth.

The runner intentionally does not validate the destination filesystem type,
free space, source hashes, or Btrfs extent layout. It writes only scenario
files and pre-created scenario directories below the supplied mount path.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

PROGRAM_VERSION = "2.0.1"
SCHEMA_VERSION = "2.0"
SOURCE_MANIFEST = "source_manifest.jsonl"
SOURCE_CONFIG = "source_config.json"
SIZE_CLASSES = ("inline", "under_chunk", "over_chunk")
CHAIN2_TRANSITIONS = (
    "under_to_inline",
    "inline_to_under",
    "over_to_under",
    "under_to_over",
)


class ScenarioError(RuntimeError):
    """Scenario execution could not be completed."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace(
        "+00:00", "Z"
    )


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ScenarioError(f"required file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ScenarioError(f"invalid JSON: {path}: {exc}") from exc


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError as exc:
        raise ScenarioError(f"required file not found: {path}") from exc
    rows: list[dict[str, Any]] = []
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ScenarioError(f"invalid JSONL at {path}:{number}: {exc}") from exc
        if not isinstance(value, dict):
            raise ScenarioError(f"JSONL record must be an object at {path}:{number}")
        rows.append(value)
    return rows


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")


def write_jsonl(path: Path, rows: Iterable[Mapping[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
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


def _stable_int(label: str) -> int:
    return int.from_bytes(hashlib.sha256(label.encode("utf-8")).digest()[:8], "big")


def _write_repeated_range(handle, pattern: bytes, length: int) -> None:
    block_repeats = max(1, (1024 * 1024) // len(pattern))
    block = pattern * block_repeats
    remaining = length
    while remaining >= len(block):
        handle.write(block)
        remaining -= len(block)
    if remaining:
        handle.write((pattern * ((remaining + len(pattern) - 1) // len(pattern)))[:remaining])


def seeded_modify(path: Path, mutation_seed: str, content_token: str) -> dict[str, Any]:
    size = path.stat().st_size
    pattern = (content_token + " ").encode("utf-8")
    if size < len(pattern):
        minimum = 1
    else:
        minimum = len(pattern)
    length = minimum + (_stable_int(mutation_seed + ":length") % (size - minimum + 1))
    offset = _stable_int(mutation_seed + f":offset:{length}") % (size - length + 1)
    with path.open("r+b") as handle:
        handle.seek(offset)
        _write_repeated_range(handle, pattern, length)
        handle.flush()
        os.fsync(handle.fileno())
    return {
        "offset": offset,
        "length": length,
        "content_token": content_token,
        "content_pattern": content_token + " ",
    }


def rewrite_in_place(path: Path, size: int, content_token: str) -> None:
    pattern = (content_token + " ").encode("utf-8")
    with path.open("r+b") as handle:
        handle.seek(0)
        _write_repeated_range(handle, pattern, size)
        handle.truncate(size)
        handle.flush()
        os.fsync(handle.fileno())


def expected_ground_truth_counts(
    *, count_per_class: int, chain2_counts: Mapping[str, int]
) -> dict[str, int]:
    chain2_total = sum(int(value) for value in chain2_counts.values())
    scenario_case_count = 6 * 3 * count_per_class + chain2_total
    return {
        "scenario_case_count": scenario_case_count,
        "historical_object_count": scenario_case_count + 3 * count_per_class + chain2_total,
        "expected_version_count": 51 * count_per_class + 5 * chain2_total,
        "event_record_count": 69 * count_per_class + 7 * chain2_total,
        "final_live_object_count": 0,
    }


@dataclass
class ObjectState:
    object_id: str
    case_id: str
    scenario_type: str
    object_role: str
    case_index: int
    initial_size_class: str
    final_size_class: str
    transition_type: str | None
    source_entry_id: str
    initial_file_name: str
    original_content_token: str
    modified_content_token: str | None = None
    resized_content_token: str | None = None
    mutation_seed: str = ""
    resize_target_size: int | None = None
    current_path: str | None = None
    current_size_class: str | None = None
    current_size: int | None = None
    current_sha256: str | None = None
    current_tokens: list[str] = field(default_factory=list)
    current_version_id: str | None = None
    version_sequence: int = -1
    initial_version_id: str | None = None
    final_version_id: str | None = None
    created_by_event_id: str | None = None
    terminated_by_event_id: str | None = None
    termination_reason: str | None = None
    live: bool = False


class ScenarioEngine:
    def __init__(
        self,
        *,
        source_dir: Path,
        dataset_root: Path,
        results_dir: Path,
        source_rows: Sequence[dict[str, Any]],
        source_config: dict[str, Any],
        run_id: str,
        pause_seconds: float,
        quiet: bool,
    ) -> None:
        self.source_dir = source_dir
        self.dataset_root = dataset_root
        self.results_dir = results_dir
        self.source_rows = list(source_rows)
        self.source_config = source_config
        self.run_id = run_id
        self.pause_seconds = pause_seconds
        self.quiet = quiet
        self.events: list[dict[str, Any]] = []
        self.versions: list[dict[str, Any]] = []
        self.version_index: dict[str, dict[str, Any]] = {}
        self.objects: dict[str, ObjectState] = {}
        self.cases: dict[str, dict[str, Any]] = {}
        self.stages: list[dict[str, Any]] = []
        self.event_counter = 0
        self.event_occurrence_counter: collections.Counter[tuple[str, str]] = collections.Counter()
        self.source_by_object = {str(row["object_id"]): row for row in source_rows}
        self.rows_by_scenario: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
        for row in source_rows:
            self.rows_by_scenario[str(row["scenario_type"])].append(row)
        for rows in self.rows_by_scenario.values():
            rows.sort(
                key=lambda row: (
                    str(row.get("initial_size_class") or ""),
                    str(row.get("transition_type") or ""),
                    int(row.get("case_index") or 0),
                    str(row.get("object_role") or ""),
                )
            )
        self._build_cases()

    def _build_cases(self) -> None:
        for row in self.source_rows:
            case_id = str(row["case_id"])
            case = self.cases.setdefault(
                case_id,
                {
                    "case_id": case_id,
                    "scenario_type": str(row["scenario_type"]),
                    "case_index": int(row["case_index"]),
                    "initial_size_class": str(row["initial_size_class"]),
                    "final_size_class": str(row["final_size_class"]),
                    "transition_type": row.get("transition_type"),
                    "object_ids": [],
                    "final_live_presence": False,
                },
            )
            object_id = str(row["object_id"])
            if object_id not in case["object_ids"]:
                case["object_ids"].append(object_id)
            if row["scenario_type"] == "chain2":
                copy_id = str(row["copy_object_id"])
                if copy_id not in case["object_ids"]:
                    case["object_ids"].append(copy_id)

    def _print(self, message: str) -> None:
        if not self.quiet:
            print(message, flush=True)

    def _next_event_id(self) -> str:
        self.event_counter += 1
        return f"EVT-{self.event_counter:08d}"

    def _relative(self, path: Path) -> str:
        return path.relative_to(self.dataset_root).as_posix()

    def _absolute(self, relative_path: str) -> Path:
        return self.dataset_root / relative_path

    def _sync(self) -> None:
        subprocess.run(["sync"], check=True)
        if self.pause_seconds:
            time.sleep(self.pause_seconds)

    def run_stage(self, stage_id: str, description: str, fn: Callable[[], None]) -> None:
        start_utc = utc_now()
        start_ns = time.time_ns()
        start_event = self.event_counter + 1
        self._print(f"{stage_id}: {description}")
        fn()
        self._sync()
        end_ns = time.time_ns()
        self.stages.append(
            {
                "stage_id": stage_id,
                "description": description,
                "start_utc": start_utc,
                "start_unix_ns": start_ns,
                "end_utc": utc_now(),
                "end_unix_ns": end_ns,
                "first_event_sequence": start_event if self.event_counter >= start_event else None,
                "last_event_sequence": self.event_counter if self.event_counter >= start_event else None,
                "event_count": max(0, self.event_counter - start_event + 1),
            }
        )

    def _make_object_from_source(self, row: dict[str, Any]) -> ObjectState:
        return ObjectState(
            object_id=str(row["object_id"]),
            case_id=str(row["case_id"]),
            scenario_type=str(row["scenario_type"]),
            object_role=str(row["object_role"]),
            case_index=int(row["case_index"]),
            initial_size_class=str(row["initial_size_class"]),
            final_size_class=str(row["final_size_class"]),
            transition_type=row.get("transition_type"),
            source_entry_id=str(row.get("source_entry_id") or ""),
            initial_file_name=str(row["file_name"]),
            original_content_token=str(row["content_token"]),
            modified_content_token=row.get("modified_content_token"),
            resized_content_token=row.get("resized_content_token"),
            mutation_seed=str(row.get("mutation_seed") or ""),
            resize_target_size=(
                int(row["resize_target_size"])
                if row.get("resize_target_size") is not None
                else None
            ),
        )

    def _make_chain2_copy(self, original: ObjectState) -> ObjectState:
        return ObjectState(
            object_id=str(self.source_by_object[original.object_id]["copy_object_id"]),
            case_id=original.case_id,
            scenario_type="chain2",
            object_role="chain2_copy",
            case_index=original.case_index,
            initial_size_class=original.initial_size_class,
            final_size_class=original.final_size_class,
            transition_type=original.transition_type,
            source_entry_id=original.source_entry_id,
            initial_file_name=original.initial_file_name,
            original_content_token=original.original_content_token,
            modified_content_token=original.modified_content_token,
            resized_content_token=original.resized_content_token,
            mutation_seed=original.mutation_seed,
            resize_target_size=original.resize_target_size,
        )

    def close_current_version(self, obj: ObjectState, event_id: str, reason: str) -> None:
        if obj.current_version_id is None:
            return
        row = self.version_index[obj.current_version_id]
        row["closed_by_event_id"] = event_id
        row["closed_reason"] = reason

    def create_version(
        self,
        obj: ObjectState,
        *,
        label: str,
        relative_path: str,
        size_class: str,
        size: int,
        sha256: str,
        content_tokens: Sequence[str],
        event_id: str,
    ) -> str:
        obj.version_sequence += 1
        version_id = f"VER-{obj.object_id[4:]}-{obj.version_sequence:02d}"
        row = {
            "version_id": version_id,
            "case_id": obj.case_id,
            "object_id": obj.object_id,
            "scenario_type": obj.scenario_type,
            "object_role": obj.object_role,
            "case_index": obj.case_index,
            "initial_size_class": obj.initial_size_class,
            "final_size_class": obj.final_size_class,
            "transition_type": obj.transition_type,
            "version_sequence": obj.version_sequence,
            "version_label": label,
            "relative_path": relative_path,
            "expected_directory": str(Path(relative_path).parent.as_posix()),
            "file_name": Path(relative_path).name,
            "size_class": size_class,
            "size_bytes": size,
            "sha256": sha256,
            "content_tokens": list(dict.fromkeys(content_tokens)),
            "created_by_event_id": event_id,
            "closed_by_event_id": None,
            "closed_reason": None,
        }
        if row["expected_directory"] == ".":
            row["expected_directory"] = ""
        self.versions.append(row)
        self.version_index[version_id] = row
        obj.current_version_id = version_id
        obj.final_version_id = version_id
        if obj.initial_version_id is None:
            obj.initial_version_id = version_id
        obj.current_path = relative_path
        obj.current_size_class = size_class
        obj.current_size = size
        obj.current_sha256 = sha256
        obj.current_tokens = list(dict.fromkeys(content_tokens))
        obj.live = True
        return version_id

    def _base_event(
        self,
        *,
        event_id: str,
        stage_id: str,
        obj: ObjectState,
        event_type: str,
        start_utc: str,
        start_ns: int,
        end_utc: str,
        end_ns: int,
        occurrence: int | None = None,
        subtype: str | None = None,
    ) -> dict[str, Any]:
        return {
            "event_id": event_id,
            "event_sequence": self.event_counter,
            "stage_id": stage_id,
            "case_id": obj.case_id,
            "scenario_type": obj.scenario_type,
            "event_type": event_type,
            "event_subtype": subtype,
            "event_occurrence": occurrence,
            "case_index": obj.case_index,
            "initial_size_class": obj.initial_size_class,
            "final_size_class": obj.final_size_class,
            "transition_type": obj.transition_type,
            "operation_start_utc": start_utc,
            "operation_start_unix_ns": start_ns,
            "operation_end_utc": end_utc,
            "operation_end_unix_ns": end_ns,
        }

    def initial_path(self, row: dict[str, Any]) -> str:
        scenario = str(row["scenario_type"])
        file_name = str(row["file_name"])
        return {
            "create": f"01_create/{file_name}",
            "modify": f"02_modify/{file_name}",
            "rename": f"03_rename/{file_name}",
            "move": f"04_move/location_0/{file_name}",
            "overwrite": f"05_overwrite/{file_name}",
            "chain1": f"06_chain1/start/{file_name}",
            "chain2": f"07_chain2/original/{file_name}",
        }[scenario]

    def create_initial(self, row: dict[str, Any], stage_id: str) -> None:
        obj = self._make_object_from_source(row)
        event_id = self._next_event_id()
        start_utc, start_ns = utc_now(), time.time_ns()
        relative_path = self.initial_path(row)
        source = self.source_dir / str(row["relative_path"])
        destination = self._absolute(relative_path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cp", "--", str(source), str(destination)], check=True)
        end_ns, end_utc = time.time_ns(), utc_now()
        version_id = self.create_version(
            obj,
            label="initial",
            relative_path=relative_path,
            size_class=str(row["initial_size_class"]),
            size=int(row["actual_size"]),
            sha256=str(row["sha256"]),
            content_tokens=[str(row["content_token"])],
            event_id=event_id,
        )
        obj.created_by_event_id = event_id
        self.objects[obj.object_id] = obj
        event = self._base_event(
            event_id=event_id,
            stage_id=stage_id,
            obj=obj,
            event_type="create",
            start_utc=start_utc,
            start_ns=start_ns,
            end_utc=end_utc,
            end_ns=end_ns,
        )
        event.update(
            {
                "object_id_before": None,
                "object_id_after": obj.object_id,
                "version_id_before": None,
                "version_id_after": version_id,
                "path_before": None,
                "path_after": relative_path,
                "size_class_before": None,
                "size_class_after": obj.current_size_class,
                "size_before": None,
                "size_after": obj.current_size,
                "sha256_before": None,
                "sha256_after": obj.current_sha256,
                "content_tokens_after": obj.current_tokens,
                "command": ["cp", "--", str(source), str(destination)],
            }
        )
        self.events.append(event)

    def modify_object(self, obj: ObjectState, stage_id: str, label: str) -> None:
        assert obj.current_path and obj.current_sha256 and obj.current_size is not None
        assert obj.current_size_class
        token = obj.modified_content_token
        if not token:
            raise ScenarioError(f"missing modified content token for {obj.object_id}")
        event_id = self._next_event_id()
        before_version = obj.current_version_id
        before_path = obj.current_path
        before_hash = obj.current_sha256
        before_size = obj.current_size
        before_class = obj.current_size_class
        start_utc, start_ns = utc_now(), time.time_ns()
        modified_range = seeded_modify(self._absolute(before_path), obj.mutation_seed, token)
        after_hash = sha256_file(self._absolute(before_path))
        end_ns, end_utc = time.time_ns(), utc_now()
        self.close_current_version(obj, event_id, "modified")
        after_version = self.create_version(
            obj,
            label=label,
            relative_path=before_path,
            size_class=before_class,
            size=before_size,
            sha256=after_hash,
            content_tokens=[obj.original_content_token, token],
            event_id=event_id,
        )
        event = self._base_event(
            event_id=event_id,
            stage_id=stage_id,
            obj=obj,
            event_type="modify",
            start_utc=start_utc,
            start_ns=start_ns,
            end_utc=end_utc,
            end_ns=end_ns,
            subtype="content_range",
        )
        event.update(
            {
                "object_id_before": obj.object_id,
                "object_id_after": obj.object_id,
                "version_id_before": before_version,
                "version_id_after": after_version,
                "path_before": before_path,
                "path_after": before_path,
                "size_class_before": before_class,
                "size_class_after": before_class,
                "size_before": before_size,
                "size_after": before_size,
                "sha256_before": before_hash,
                "sha256_after": after_hash,
                "modified_ranges": [modified_range],
                "content_tokens_after": obj.current_tokens,
            }
        )
        self.events.append(event)

    def rename_object(
        self,
        obj: ObjectState,
        *,
        stage_id: str,
        new_file_name: str,
        occurrence: int,
        label: str,
    ) -> None:
        assert obj.current_path and obj.current_sha256 and obj.current_size is not None
        assert obj.current_size_class and obj.current_version_id
        event_id = self._next_event_id()
        before_path = obj.current_path
        after_path = (Path(before_path).parent / new_file_name).as_posix()
        start_utc, start_ns = utc_now(), time.time_ns()
        os.replace(self._absolute(before_path), self._absolute(after_path))
        end_ns, end_utc = time.time_ns(), utc_now()
        before_version = obj.current_version_id
        self.close_current_version(obj, event_id, "renamed")
        after_version = self.create_version(
            obj,
            label=label,
            relative_path=after_path,
            size_class=obj.current_size_class,
            size=obj.current_size,
            sha256=obj.current_sha256,
            content_tokens=obj.current_tokens,
            event_id=event_id,
        )
        event = self._base_event(
            event_id=event_id,
            stage_id=stage_id,
            obj=obj,
            event_type="rename",
            start_utc=start_utc,
            start_ns=start_ns,
            end_utc=end_utc,
            end_ns=end_ns,
            occurrence=occurrence,
        )
        event.update(
            {
                "object_id_before": obj.object_id,
                "object_id_after": obj.object_id,
                "version_id_before": before_version,
                "version_id_after": after_version,
                "path_before": before_path,
                "path_after": after_path,
                "size_class_before": obj.current_size_class,
                "size_class_after": obj.current_size_class,
                "size_before": obj.current_size,
                "size_after": obj.current_size,
                "sha256_before": obj.current_sha256,
                "sha256_after": obj.current_sha256,
            }
        )
        self.events.append(event)

    def move_object(
        self,
        obj: ObjectState,
        *,
        stage_id: str,
        destination_dir: str,
        occurrence: int,
        label: str,
    ) -> None:
        assert obj.current_path and obj.current_sha256 and obj.current_size is not None
        assert obj.current_size_class and obj.current_version_id
        event_id = self._next_event_id()
        before_path = obj.current_path
        after_path = (Path(destination_dir) / Path(before_path).name).as_posix()
        start_utc, start_ns = utc_now(), time.time_ns()
        os.replace(self._absolute(before_path), self._absolute(after_path))
        end_ns, end_utc = time.time_ns(), utc_now()
        before_version = obj.current_version_id
        self.close_current_version(obj, event_id, "moved")
        after_version = self.create_version(
            obj,
            label=label,
            relative_path=after_path,
            size_class=obj.current_size_class,
            size=obj.current_size,
            sha256=obj.current_sha256,
            content_tokens=obj.current_tokens,
            event_id=event_id,
        )
        event = self._base_event(
            event_id=event_id,
            stage_id=stage_id,
            obj=obj,
            event_type="move",
            start_utc=start_utc,
            start_ns=start_ns,
            end_utc=end_utc,
            end_ns=end_ns,
            occurrence=occurrence,
        )
        event.update(
            {
                "object_id_before": obj.object_id,
                "object_id_after": obj.object_id,
                "version_id_before": before_version,
                "version_id_after": after_version,
                "path_before": before_path,
                "path_after": after_path,
                "size_class_before": obj.current_size_class,
                "size_class_after": obj.current_size_class,
                "size_before": obj.current_size,
                "size_after": obj.current_size,
                "sha256_before": obj.current_sha256,
                "sha256_after": obj.current_sha256,
            }
        )
        self.events.append(event)

    def delete_object(self, obj: ObjectState, *, stage_id: str, reason: str) -> None:
        assert obj.current_path and obj.current_version_id
        event_id = self._next_event_id()
        before_path = obj.current_path
        before_version = obj.current_version_id
        before_size = obj.current_size
        before_class = obj.current_size_class
        before_hash = obj.current_sha256
        start_utc, start_ns = utc_now(), time.time_ns()
        self._absolute(before_path).unlink()
        end_ns, end_utc = time.time_ns(), utc_now()
        self.close_current_version(obj, event_id, reason)
        obj.live = False
        obj.current_path = None
        obj.current_version_id = None
        obj.terminated_by_event_id = event_id
        obj.termination_reason = reason
        event = self._base_event(
            event_id=event_id,
            stage_id=stage_id,
            obj=obj,
            event_type="delete",
            start_utc=start_utc,
            start_ns=start_ns,
            end_utc=end_utc,
            end_ns=end_ns,
        )
        event.update(
            {
                "object_id_before": obj.object_id,
                "object_id_after": None,
                "version_id_before": before_version,
                "version_id_after": None,
                "path_before": before_path,
                "path_after": None,
                "size_class_before": before_class,
                "size_class_after": None,
                "size_before": before_size,
                "size_after": None,
                "sha256_before": before_hash,
                "sha256_after": None,
            }
        )
        self.events.append(event)

    def overwrite_case(self, incoming: ObjectState, target_row: dict[str, Any], stage_id: str) -> None:
        assert incoming.current_path and incoming.current_version_id
        event_id = self._next_event_id()
        before_path = incoming.current_path
        before_version = incoming.current_version_id
        before_size = incoming.current_size
        before_hash = incoming.current_sha256
        before_class = incoming.current_size_class
        source = self.source_dir / str(target_row["relative_path"])
        destination = self._absolute(before_path)
        start_utc, start_ns = utc_now(), time.time_ns()
        subprocess.run(
            ["cp", "--remove-destination", "--", str(source), str(destination)],
            check=True,
        )
        end_ns, end_utc = time.time_ns(), utc_now()
        self.close_current_version(incoming, event_id, "overwritten")
        incoming.live = False
        incoming.current_path = None
        incoming.current_version_id = None
        incoming.terminated_by_event_id = event_id
        incoming.termination_reason = "overwritten"

        target = self._make_object_from_source(target_row)
        target.created_by_event_id = event_id
        target_version = self.create_version(
            target,
            label="post_overwrite",
            relative_path=before_path,
            size_class=str(target_row["initial_size_class"]),
            size=int(target_row["actual_size"]),
            sha256=str(target_row["sha256"]),
            content_tokens=[str(target_row["content_token"])],
            event_id=event_id,
        )
        self.objects[target.object_id] = target
        event = self._base_event(
            event_id=event_id,
            stage_id=stage_id,
            obj=target,
            event_type="overwrite",
            start_utc=start_utc,
            start_ns=start_ns,
            end_utc=end_utc,
            end_ns=end_ns,
        )
        event.update(
            {
                "object_id_before": incoming.object_id,
                "object_id_after": target.object_id,
                "replaced_object_id": incoming.object_id,
                "replacement_object_id": target.object_id,
                "version_id_before": before_version,
                "version_id_after": target_version,
                "path_before": before_path,
                "path_after": before_path,
                "size_class_before": before_class,
                "size_class_after": target.current_size_class,
                "size_before": before_size,
                "size_after": target.current_size,
                "sha256_before": before_hash,
                "sha256_after": target.current_sha256,
                "incoming_content_token": incoming.original_content_token,
                "target_content_token": target.original_content_token,
                "command": [
                    "cp",
                    "--remove-destination",
                    "--",
                    str(source),
                    str(destination),
                ],
            }
        )
        self.events.append(event)

    def copy_chain2(self, original: ObjectState, stage_id: str) -> None:
        assert original.current_path and original.current_version_id
        assert original.current_sha256 and original.current_size is not None
        assert original.current_size_class
        event_id = self._next_event_id()
        copy_obj = self._make_chain2_copy(original)
        copy_path = (Path("07_chain2/copy") / Path(original.current_path).name).as_posix()
        start_utc, start_ns = utc_now(), time.time_ns()
        subprocess.run(
            ["cp", "--", str(self._absolute(original.current_path)), str(self._absolute(copy_path))],
            check=True,
        )
        end_ns, end_utc = time.time_ns(), utc_now()
        copy_version = self.create_version(
            copy_obj,
            label="copy_created",
            relative_path=copy_path,
            size_class=original.current_size_class,
            size=original.current_size,
            sha256=original.current_sha256,
            content_tokens=original.current_tokens,
            event_id=event_id,
        )
        copy_obj.created_by_event_id = event_id
        self.objects[copy_obj.object_id] = copy_obj
        event = self._base_event(
            event_id=event_id,
            stage_id=stage_id,
            obj=original,
            event_type="copy",
            start_utc=start_utc,
            start_ns=start_ns,
            end_utc=end_utc,
            end_ns=end_ns,
        )
        event.update(
            {
                "object_id_before": original.object_id,
                "object_id_after": copy_obj.object_id,
                "source_object_id": original.object_id,
                "created_object_id": copy_obj.object_id,
                "version_id_before": original.current_version_id,
                "version_id_after": copy_version,
                "source_version_id": original.current_version_id,
                "created_version_id": copy_version,
                "path_before": original.current_path,
                "path_after": copy_path,
                "size_class_before": original.current_size_class,
                "size_class_after": copy_obj.current_size_class,
                "size_before": original.current_size,
                "size_after": copy_obj.current_size,
                "sha256_before": original.current_sha256,
                "sha256_after": copy_obj.current_sha256,
            }
        )
        self.events.append(event)

    def resize_chain2(self, obj: ObjectState, stage_id: str) -> None:
        assert obj.current_path and obj.current_version_id
        assert obj.current_sha256 and obj.current_size is not None
        assert obj.current_size_class and obj.resize_target_size is not None
        assert obj.resized_content_token
        event_id = self._next_event_id()
        before_path = obj.current_path
        before_version = obj.current_version_id
        before_hash = obj.current_sha256
        before_size = obj.current_size
        before_class = obj.current_size_class
        start_utc, start_ns = utc_now(), time.time_ns()
        rewrite_in_place(
            self._absolute(before_path), obj.resize_target_size, obj.resized_content_token
        )
        after_hash = sha256_file(self._absolute(before_path))
        end_ns, end_utc = time.time_ns(), utc_now()
        self.close_current_version(obj, event_id, "size_class_changed")
        after_version = self.create_version(
            obj,
            label="copy_post_resize",
            relative_path=before_path,
            size_class=obj.final_size_class,
            size=obj.resize_target_size,
            sha256=after_hash,
            content_tokens=[obj.resized_content_token],
            event_id=event_id,
        )
        event = self._base_event(
            event_id=event_id,
            stage_id=stage_id,
            obj=obj,
            event_type="modify",
            subtype="size_class_change",
            start_utc=start_utc,
            start_ns=start_ns,
            end_utc=end_utc,
            end_ns=end_ns,
        )
        event.update(
            {
                "object_id_before": obj.object_id,
                "object_id_after": obj.object_id,
                "version_id_before": before_version,
                "version_id_after": after_version,
                "path_before": before_path,
                "path_after": before_path,
                "size_class_before": before_class,
                "size_class_after": obj.final_size_class,
                "size_before": before_size,
                "size_after": obj.resize_target_size,
                "sha256_before": before_hash,
                "sha256_after": after_hash,
                "resize_direction": "shrink" if obj.resize_target_size < before_size else "grow",
                "content_tokens_after": obj.current_tokens,
            }
        )
        self.events.append(event)

    def _objects_for(self, scenario: str, role: str | None = None) -> list[ObjectState]:
        result = [
            obj
            for obj in self.objects.values()
            if obj.scenario_type == scenario and (role is None or obj.object_role == role)
        ]
        return sorted(
            result,
            key=lambda obj: (
                SIZE_CLASSES.index(obj.initial_size_class),
                obj.transition_type or "",
                obj.case_index,
                obj.object_role,
            ),
        )

    def execute(self) -> dict[str, Any]:
        stage_dirs = [
            "01_create",
            "02_modify",
            "03_rename",
            "04_move/location_0",
            "04_move/location_1",
            "04_move/location_2",
            "04_move/location_3",
            "05_overwrite",
            "06_chain1/start",
            "06_chain1/final",
            "07_chain2/original",
            "07_chain2/copy",
        ]

        self.run_stage(
            "S00_PREPARE_DIRECTORIES",
            "pre-create all scenario directories",
            lambda: [self._absolute(directory).mkdir(parents=True, exist_ok=True) for directory in stage_dirs],
        )

        def initial_create() -> None:
            rows = [row for row in self.source_rows if bool(row.get("btrfs_initial_copy"))]
            scenario_order = ("create", "modify", "rename", "move", "overwrite", "chain1", "chain2")
            transition_order = (
                "under_to_inline",
                "inline_to_under",
                "over_to_under",
                "under_to_over",
            )
            rows.sort(
                key=lambda row: (
                    scenario_order.index(str(row["scenario_type"])),
                    int(row["case_index"]),
                    SIZE_CLASSES.index(str(row["initial_size_class"])),
                    transition_order.index(str(row["transition_type"]))
                    if row.get("transition_type")
                    else -1,
                )
            )
            for row in rows:
                self.create_initial(row, "S01_INITIAL_CREATE")

        self.run_stage("S01_INITIAL_CREATE", "copy all initial scenario objects", initial_create)

        def initial_modify() -> None:
            for scenario in ("modify", "chain1", "chain2"):
                for obj in self._objects_for(scenario, "chain2_original" if scenario == "chain2" else "subject"):
                    label = {
                        "modify": "post_modify",
                        "chain1": "post_modify",
                        "chain2": "original_post_modify",
                    }[scenario]
                    self.modify_object(obj, "S02_INITIAL_MODIFY", label)

        self.run_stage("S02_INITIAL_MODIFY", "modify modify, chain1, and chain2 originals", initial_modify)

        self.run_stage(
            "S03_RENAME_1",
            "rename scenario occurrence 1",
            lambda: [
                self.rename_object(
                    obj,
                    stage_id="S03_RENAME_1",
                    new_file_name=f"renamed_1_{obj.initial_size_class}_{obj.case_index:03d}.txt",
                    occurrence=1,
                    label="post_rename_1",
                )
                for obj in self._objects_for("rename")
            ],
        )

        self.run_stage(
            "S04_MOVE_1",
            "move scenario occurrence 1",
            lambda: [
                self.move_object(
                    obj,
                    stage_id="S04_MOVE_1",
                    destination_dir="04_move/location_1",
                    occurrence=1,
                    label="post_move_1",
                )
                for obj in self._objects_for("move")
            ],
        )

        self.run_stage(
            "S05_CREATE_DELETE",
            "delete create scenario objects",
            lambda: [
                self.delete_object(obj, stage_id="S05_CREATE_DELETE", reason="scenario_complete")
                for obj in self._objects_for("create")
            ],
        )

        def overwrite_stage() -> None:
            target_rows = {
                str(row["case_id"]): row
                for row in self.rows_by_scenario["overwrite"]
                if row["object_role"] == "overwrite_target"
            }
            for incoming in self._objects_for("overwrite", "overwrite_incoming"):
                self.overwrite_case(incoming, target_rows[incoming.case_id], "S06_OVERWRITE")

        self.run_stage("S06_OVERWRITE", "replace incoming objects with external targets", overwrite_stage)

        def rename2_chain1() -> None:
            for obj in self._objects_for("rename"):
                self.rename_object(
                    obj,
                    stage_id="S07_RENAME_2_CHAIN1_RENAME",
                    new_file_name=f"renamed_2_{obj.initial_size_class}_{obj.case_index:03d}.txt",
                    occurrence=2,
                    label="post_rename_2",
                )
            for obj in self._objects_for("chain1"):
                self.rename_object(
                    obj,
                    stage_id="S07_RENAME_2_CHAIN1_RENAME",
                    new_file_name=f"chain1_renamed_{obj.initial_size_class}_{obj.case_index:03d}.txt",
                    occurrence=1,
                    label="post_rename",
                )

        self.run_stage("S07_RENAME_2_CHAIN1_RENAME", "rename occurrence 2 and chain1 rename", rename2_chain1)

        self.run_stage(
            "S08_CHAIN2_COPY",
            "copy modified chain2 originals",
            lambda: [self.copy_chain2(obj, "S08_CHAIN2_COPY") for obj in self._objects_for("chain2", "chain2_original")],
        )

        self.run_stage(
            "S09_CHAIN2_ORIGINAL_DELETE",
            "delete chain2 originals",
            lambda: [
                self.delete_object(obj, stage_id="S09_CHAIN2_ORIGINAL_DELETE", reason="original_deleted_after_copy")
                for obj in self._objects_for("chain2", "chain2_original")
            ],
        )

        def move2_chain1() -> None:
            for obj in self._objects_for("move"):
                self.move_object(
                    obj,
                    stage_id="S10_MOVE_2_CHAIN1_MOVE",
                    destination_dir="04_move/location_2",
                    occurrence=2,
                    label="post_move_2",
                )
            for obj in self._objects_for("chain1"):
                self.move_object(
                    obj,
                    stage_id="S10_MOVE_2_CHAIN1_MOVE",
                    destination_dir="06_chain1/final",
                    occurrence=1,
                    label="post_move",
                )

        self.run_stage("S10_MOVE_2_CHAIN1_MOVE", "move occurrence 2 and chain1 move", move2_chain1)

        self.run_stage(
            "S11_MODIFY_DELETE",
            "delete modify scenario objects",
            lambda: [
                self.delete_object(obj, stage_id="S11_MODIFY_DELETE", reason="scenario_complete")
                for obj in self._objects_for("modify")
            ],
        )

        self.run_stage(
            "S12_CHAIN2_COPY_RENAME",
            "rename chain2 copies",
            lambda: [
                self.rename_object(
                    obj,
                    stage_id="S12_CHAIN2_COPY_RENAME",
                    new_file_name=f"chain2_renamed_{obj.transition_type}_{obj.case_index:03d}.txt",
                    occurrence=1,
                    label="copy_post_rename",
                )
                for obj in self._objects_for("chain2", "chain2_copy")
            ],
        )

        self.run_stage(
            "S13_RENAME_3",
            "rename scenario occurrence 3",
            lambda: [
                self.rename_object(
                    obj,
                    stage_id="S13_RENAME_3",
                    new_file_name=f"renamed_3_{obj.initial_size_class}_{obj.case_index:03d}.txt",
                    occurrence=3,
                    label="post_rename_3",
                )
                for obj in self._objects_for("rename")
            ],
        )

        self.run_stage(
            "S14_CHAIN2_SIZE_CLASS_CHANGE",
            "resize chain2 copies in place",
            lambda: [self.resize_chain2(obj, "S14_CHAIN2_SIZE_CLASS_CHANGE") for obj in self._objects_for("chain2", "chain2_copy")],
        )

        self.run_stage(
            "S15_OVERWRITE_TARGET_DELETE",
            "delete overwrite target objects",
            lambda: [
                self.delete_object(obj, stage_id="S15_OVERWRITE_TARGET_DELETE", reason="scenario_complete")
                for obj in self._objects_for("overwrite", "overwrite_target")
            ],
        )

        self.run_stage(
            "S16_MOVE_3",
            "move scenario occurrence 3",
            lambda: [
                self.move_object(
                    obj,
                    stage_id="S16_MOVE_3",
                    destination_dir="04_move/location_3",
                    occurrence=3,
                    label="post_move_3",
                )
                for obj in self._objects_for("move")
            ],
        )

        self.run_stage(
            "S17_CHAIN1_DELETE",
            "delete chain1 objects",
            lambda: [
                self.delete_object(obj, stage_id="S17_CHAIN1_DELETE", reason="scenario_complete")
                for obj in self._objects_for("chain1")
            ],
        )
        self.run_stage(
            "S18_RENAME_DELETE",
            "delete rename scenario objects",
            lambda: [
                self.delete_object(obj, stage_id="S18_RENAME_DELETE", reason="scenario_complete")
                for obj in self._objects_for("rename")
            ],
        )
        self.run_stage(
            "S19_MOVE_DELETE",
            "delete move scenario objects",
            lambda: [
                self.delete_object(obj, stage_id="S19_MOVE_DELETE", reason="scenario_complete")
                for obj in self._objects_for("move")
            ],
        )
        self.run_stage(
            "S20_CHAIN2_COPY_DELETE",
            "delete resized chain2 copies",
            lambda: [
                self.delete_object(obj, stage_id="S20_CHAIN2_COPY_DELETE", reason="scenario_complete")
                for obj in self._objects_for("chain2", "chain2_copy")
            ],
        )

        return self.write_results()

    def write_results(self) -> dict[str, Any]:
        object_rows: list[dict[str, Any]] = []
        for obj in sorted(self.objects.values(), key=lambda item: item.object_id):
            object_rows.append(
                {
                    "object_id": obj.object_id,
                    "case_id": obj.case_id,
                    "scenario_type": obj.scenario_type,
                    "object_role": obj.object_role,
                    "case_index": obj.case_index,
                    "initial_size_class": obj.initial_size_class,
                    "final_size_class": obj.final_size_class,
                    "transition_type": obj.transition_type,
                    "source_entry_id": obj.source_entry_id,
                    "file_name": obj.initial_file_name,
                    "content_token": obj.original_content_token,
                    "modified_content_token": obj.modified_content_token,
                    "resized_content_token": obj.resized_content_token,
                    "initial_version_id": obj.initial_version_id,
                    "final_version_id": obj.final_version_id,
                    "created_by_event_id": obj.created_by_event_id,
                    "terminated_by_event_id": obj.terminated_by_event_id,
                    "termination_reason": obj.termination_reason,
                    "final_live_presence": obj.live,
                    "final_relative_path": obj.current_path,
                }
            )

        case_rows = sorted(self.cases.values(), key=lambda row: row["case_id"])
        version_rows = sorted(
            self.versions,
            key=lambda row: (row["case_id"], row["object_id"], row["version_sequence"]),
        )
        event_rows = sorted(self.events, key=lambda row: row["event_sequence"])

        scenario_case_counts: dict[str, Any] = collections.defaultdict(lambda: collections.defaultdict(int))
        for case in case_rows:
            qualifier = case["transition_type"] or case["initial_size_class"]
            scenario_case_counts[case["scenario_type"]][qualifier] += 1
        expected_version_counts: dict[str, Any] = collections.defaultdict(lambda: collections.defaultdict(int))
        for version in version_rows:
            qualifier = version["transition_type"] or version["initial_size_class"]
            expected_version_counts[version["scenario_type"]][qualifier] += 1
        event_counts: dict[str, Any] = collections.defaultdict(lambda: collections.defaultdict(int))
        for event in event_rows:
            key = event["event_type"]
            if event.get("event_occurrence"):
                key += f"_occurrence_{event['event_occurrence']}"
            if event.get("event_subtype"):
                key += f"_{event['event_subtype']}"
            event_counts[event["scenario_type"]][key] += 1

        expected_counts = {
            "schema_version": SCHEMA_VERSION,
            "scenario_case_counts": {key: dict(value) for key, value in scenario_case_counts.items()},
            "expected_version_counts": {key: dict(value) for key, value in expected_version_counts.items()},
            "event_occurrence_counts": {key: dict(value) for key, value in event_counts.items()},
        }
        live_rows = [obj for obj in object_rows if obj["final_live_presence"]]
        summary = {
            "program_version": PROGRAM_VERSION,
            "schema_version": SCHEMA_VERSION,
            "status": "completed",
            "run_id": self.run_id,
            "source_dir": str(self.source_dir),
            "dataset_root": str(self.dataset_root),
            "results_dir": str(self.results_dir),
            "scenario_case_count": len(case_rows),
            "historical_object_count": len(object_rows),
            "expected_version_count": len(version_rows),
            "event_record_count": len(event_rows),
            "final_live_object_count": len(live_rows),
            "completed_at_utc": utc_now(),
        }
        write_jsonl(self.results_dir / "case_manifest.jsonl", case_rows)
        write_jsonl(self.results_dir / "object_manifest.jsonl", object_rows)
        write_jsonl(self.results_dir / "version_manifest.jsonl", version_rows)
        write_jsonl(self.results_dir / "ground_truth_events.jsonl", event_rows)
        write_json(self.results_dir / "expected_counts.json", expected_counts)
        write_json(self.results_dir / "stages.json", self.stages)
        write_json(self.results_dir / "run_config.json", summary)
        write_jsonl(self.results_dir / "final_inventory.jsonl", live_rows)
        return summary


def run_scenario(
    *,
    source_dir: Path,
    mount_path: Path,
    results_dir: Path | None = None,
    run_id: str = "evaluation-001",
    pause_seconds: float = 0.05,
    quiet: bool = False,
) -> dict[str, Any]:
    source_dir = Path(source_dir).expanduser().resolve()
    mount_path = Path(mount_path).expanduser().resolve()
    source_config = read_json(source_dir / SOURCE_CONFIG)
    if str(source_config.get("schema_version")) != SCHEMA_VERSION:
        raise ScenarioError(
            f"unsupported source schema {source_config.get('schema_version')!r}; expected {SCHEMA_VERSION}"
        )
    source_rows = read_jsonl(source_dir / SOURCE_MANIFEST)
    if results_dir is None:
        results_base = source_dir.with_name(source_dir.name + "_results")
    else:
        results_base = Path(results_dir).expanduser().resolve()
    dataset_root = mount_path / f"BTRFS_EVAL_{run_id}"
    run_results = results_base / run_id
    if dataset_root.exists():
        raise ScenarioError(f"dataset path already exists: {dataset_root}")
    if run_results.exists():
        raise ScenarioError(f"result path already exists: {run_results}")
    mount_path.mkdir(parents=True, exist_ok=True)
    dataset_root.mkdir()
    run_results.mkdir(parents=True)

    engine = ScenarioEngine(
        source_dir=source_dir,
        dataset_root=dataset_root,
        results_dir=run_results,
        source_rows=source_rows,
        source_config=source_config,
        run_id=run_id,
        pause_seconds=pause_seconds,
        quiet=quiet,
    )
    try:
        summary = engine.execute()
    except Exception as exc:
        write_json(
            run_results / "run_config.json",
            {
                "program_version": PROGRAM_VERSION,
                "schema_version": SCHEMA_VERSION,
                "status": "failed",
                "run_id": run_id,
                "source_dir": str(source_dir),
                "dataset_root": str(dataset_root),
                "results_dir": str(run_results),
                "error": f"{type(exc).__name__}: {exc}",
                "failed_at_utc": utc_now(),
            },
        )
        raise
    if not quiet:
        print(json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True))
    return summary


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the schema-2.0 Btrfs file-event evaluation scenario."
    )
    parser.add_argument("source_dir", type=Path)
    parser.add_argument("btrfs_mount", type=Path)
    parser.add_argument("--results-dir", type=Path)
    parser.add_argument("--run-id", default="evaluation-001")
    parser.add_argument("--pause-seconds", type=float, default=0.05)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--version", action="version", version=f"%(prog)s {PROGRAM_VERSION}")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    if args.pause_seconds < 0:
        parser.error("--pause-seconds must be zero or greater")
    try:
        run_scenario(
            source_dir=args.source_dir,
            mount_path=args.btrfs_mount,
            results_dir=args.results_dir,
            run_id=args.run_id,
            pause_seconds=args.pause_seconds,
            quiet=args.quiet,
        )
    except (ScenarioError, OSError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
