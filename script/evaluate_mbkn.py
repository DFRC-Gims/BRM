#!/usr/bin/env python3
"""Evaluate a fixed mbkn restore pool with the unchanged recovery_verify 3.0.2 core.

Only restore TSV rows with type=file and action written/reconstructed/exists can
enter the pool. TSV paths are relative to the TSV's parent (restore destination).
No directory walk, commercial duplicate parsing, or BRM prefix stripping is used.

Compatibility shim: evaluate() only accepts hetman/proposed, so this standalone,
single-threaded process temporarily replaces evidence.inventory, calls the
hetman/all_candidates branch, and restores inventory in finally. That branch's
selection is identity; EvidenceFile values have duplicate_number=None. The core
GT loader, attribution, cross-name exact-content matching, measurement and ranking
are unchanged. The BRM-specific audit is never called. Outputs identify mbkn and
record this internal routing explicitly. Do not import/run concurrently with
another user of recovery_verify in the same process.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import importlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import stat
import sys

sys.dont_write_bytecode = True
TOOL = "mbkn-btrfs-rescue"
ADAPTER_VERSION = "1.0"
COMMIT = "c2eddfbc610b23a85e5f04d960d6994f998299e8"
ACTIONS = {"written", "reconstructed", "exists"}
CLASS_NAMES = {"complete": "EM", "partial_extent": "PM", "content_mismatch": "DC",
               "metadata_only": "EO", "unresolve": "NC"}
CORE_PINS = {
    "__init__.py": "38218bd666eb6a15c55a400988329051c4475cf95011cdbd02d8835431983545",
    "cli.py": "dd5f5635778e72ef7079afe55204e26a917c58b1e9487f8a8e4d9a7dba6eaeaf",
    "evaluator.py": "4bc0eaf5efbf0007777ec7cfa3bc99117cedb3cde62e19c7b3d6a0ffe7234edf",
    "evidence.py": "12fe470330af23dbcb7d207dc2635d4b52bd08c77d01fe3d7403f260cfb068cb",
    "ground_truth.py": "b839ff8e20878c828055b70a0870549fb7453320333e978e3efe0a10b0115a3b",
    "measurement.py": "7ab67347d941e405d19315faa1503175ce49be2272dbcfc3a4231b74aae00465",
}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def snapshot(path):
    s = Path(path).stat()
    return {"size": s.st_size, "mtime_ns": s.st_mtime_ns, "ctime_ns": s.st_ctime_ns,
            "device": s.st_dev, "inode": s.st_ino}


def reject_links(path):
    """Reject symlinks/junctions on the complete supplied path, not just the leaf."""
    for p in (path, *path.parents):
        if p.is_symlink() or (hasattr(p, "is_junction") and p.is_junction()):
            raise ValueError(f"symlink/junction input is not supported: {p}")


def checked_path(path):
    p = Path(os.path.abspath(Path(path).expanduser()))
    reject_links(p)
    return p.resolve(strict=True)


def load_core(verifier_root):
    root = checked_path(verifier_root)
    package = root / "recovery_verify"
    hashes = {name: digest(package / name) for name in CORE_PINS}
    if hashes != CORE_PINS:
        raise ValueError("verifier source differs from the audited 3.0.2 pins")
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    modules = {name: importlib.import_module("recovery_verify" + ("." + name if name else ""))
               for name in ("", "evaluator", "evidence", "ground_truth")}
    for module in modules.values():
        if Path(module.__file__).resolve().parent != package.resolve():
            raise ValueError("another recovery_verify package is already loaded")
    if modules[""].__version__ != "3.0.2":
        raise ValueError("this adapter requires recovery_verify 3.0.2")
    return modules, hashes


def read_allowlist(recovery_root, manifests, evidence):
    root = checked_path(recovery_root)
    if not root.is_dir():
        raise ValueError("recovery root is not a directory")
    manifest_paths = sorted({checked_path(p) for p in manifests})
    if not manifest_paths:
        raise ValueError("at least one explicit restore manifest is required")
    for path in manifest_paths:
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError("restore manifests must be regular files inside recovery_root")
    selected, excluded, metadata = {}, [], []
    required = ["path", "type", "size", "category", "action", "tree", "ino", "detail"]
    for manifest in manifest_paths:
        before = snapshot(manifest)
        raw = manifest.read_bytes()
        metadata.append({"path": str(manifest), "sha256": hashlib.sha256(raw).hexdigest(),
                         "stat": before})
        # Splitlines would corrupt quoted paths/details with embedded newlines.
        import io
        reader = csv.DictReader(io.StringIO(raw.decode("utf-8-sig"), newline=""), delimiter="\t")
        if reader.fieldnames != required:
            raise ValueError(f"unexpected restore TSV header: {manifest}")
        for row_number, row in enumerate(reader, 2):
            if None in row or any(v is None for v in row.values()):
                raise ValueError(f"malformed TSV row {row_number}: {manifest}")
            origin = {"manifest": str(manifest), "row": row_number, **row}
            if row["type"] != "file" or row["action"] not in ACTIONS:
                excluded.append({**origin, "reason": "not an exported regular-file action"})
                continue
            rel = PurePosixPath(row["path"])
            if (not row["path"] or rel.is_absolute() or ".." in rel.parts
                    or "\\" in row["path"] or PureWindowsPath(row["path"]).drive
                    or not rel.parts or any(":" in part for part in rel.parts)):
                raise ValueError(f"unsafe or unsupported TSV path at row {row_number}: {manifest}")
            candidate = checked_path(manifest.parent.joinpath(*rel.parts))
            if not candidate.is_relative_to(root) or candidate in manifest_paths:
                raise ValueError("candidate escapes recovery_root or refers to a restore report")
            if not stat.S_ISREG(candidate.stat().st_mode):
                raise ValueError(f"manifest file row does not reference a regular file: {candidate}")
            observed = snapshot(candidate)
            if row["action"] in {"written", "reconstructed"}:
                if not row["size"].isdigit() or int(row["size"]) != observed["size"]:
                    raise ValueError(f"written size differs from the current file: {candidate}")
            elif row["size"] and (not row["size"].isdigit() or int(row["size"]) != observed["size"]):
                raise ValueError(f"exists row size differs from the current file: {candidate}")
            relative = candidate.relative_to(root).as_posix()
            if candidate in selected:
                if selected[candidate]["stat"] != observed:
                    raise ValueError(f"candidate changed while reading manifests: {candidate}")
                selected[candidate]["restore_rows"].append(origin)
            else:
                selected[candidate] = {"path": str(candidate), "relative_path": relative,
                                       "stat": observed, "restore_rows": [origin]}
        if before != snapshot(manifest):
            raise ValueError(f"manifest changed during inventory: {manifest}")
    entries = sorted(selected.values(), key=lambda r: r["relative_path"])
    files = [evidence.EvidenceFile(Path(r["path"]), r["relative_path"], r["relative_path"],
                                  Path(r["path"]).name, Path(r["path"]).name,
                                  None, r["relative_path"], None) for r in entries]
    ignored = [{"relative_path": r["path"], "recovery_relative_path": r["path"],
                "inventory_status": "ignored", "reason": r["reason"]} for r in excluded]
    return root, files, ignored, {"manifests": metadata, "files": entries,
                                  "excluded_rows": excluded, "candidate_count": len(files),
                                  "unlisted_files": "not enumerated or opened; outside the allowlist"}


def evaluate_pool(modules, gt, root, output, files, ignored, policy, progress=None):
    evidence, core = modules["evidence"], modules["evaluator"]
    previous = evidence.inventory

    def manifest_inventory(requested_root, dataset_root_name, *, parse_duplicates=True):
        if Path(requested_root).resolve() != root:
            raise ValueError("core requested an unexpected recovery root")
        # Intentionally ignore parse_duplicates: all names are literal mbkn names.
        return files, ignored

    evidence.inventory = manifest_inventory
    try:
        report = core.evaluate(gt, root, output, modes=("all_candidates",),
                               recovery_tool="hetman", cache_db=None,
                               progress=progress, write_summary=False)["all_candidates"]
    finally:
        evidence.inventory = previous
    report["recovery_tool"] = TOOL
    report["selection_mode"] = policy
    for row in (*report["targets"], *report["candidates"]):
        row["selection_mode"] = policy
        row["recovery_class"] = CLASS_NAMES[row["status"]]
    report["summary"]["classes"] = {label: report["summary"][status]
                                      for status, label in CLASS_NAMES.items()}
    report["policy"].update({
        "candidate_pool": policy,
        "pool_scope": "explicit exported restore files; not all historical generations",
        "policy_basis": "CLI-declared policy; link the independent recovery run provenance to verify options",
        "mbkn_output": "best + include all categories + include stale + no exclude" if policy == "best_include_all_stale" else "latest + include all categories + include stale + no exclude",
        "filename_normalization": "none; no suffix parsing or BRM prefix/wrapper removal",
        "manifest_actions": sorted(ACTIONS),
        "exists": "included as pre-existing output; no written-size assertion when TSV size is empty",
        "recovery_errors": "restore detail is preserved; damaged output is still measured",
        "internal_core_route": "hetman/all_candidates only; inventory overridden; no BRM audit",
        "inventory_patch": "process-local evidence.inventory replacement in try/finally; single-threaded only",
        "incomplete_NC": "unresolve with read errors is unverified, not an established missing candidate",
    })
    return report


def write_json(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def write_csv(path, rows):
    fields = list(dict.fromkeys(k for row in rows for k in row)) or [
        "object_id", "recovery_relative_path", "status", "recovery_class",
        "adapter_evaluation_complete", "input_metadata_stable", "adapter_invalidation"]
    with Path(path).open("x", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: json.dumps(v, ensure_ascii=False, sort_keys=True)
                             if isinstance(v, (dict, list, tuple)) else v for k, v in row.items()})


def apply_validity(report, stable):
    complete = stable and not report["summary"]["unverified_target_count"] and not report["inventory_read_error_count"]
    report["adapter_evaluation_complete"] = complete
    for group in [report["summary"], *report["summary_by_group"], *report["summary_by_termination"].values()]:
        group.update(adapter_evaluation_complete=complete, input_metadata_stable=stable)
        if not stable:
            report["adapter_invalidation"] = "input/source changed; observed classes are not final results"
            group["adapter_invalidation"] = report["adapter_invalidation"]
            for field in ("byte_recovery_rate", "mean_file_byte_recovery_rate", "complete_file_rate"):
                group[field] = None
    if not stable:
        for target in report["targets"]:
            target["evaluation_complete"] = False
            target["byte_recovery_rate"] = None
    return complete


def persist_report(output, report, inventory, provenance, core):
    write_json(output / "report.json", report)
    write_json(output / "targets.json", report["targets"])
    write_json(output / "candidates.json", report["candidates"])
    write_json(output / "inventory.json", inventory)
    write_json(output / "provenance.json", provenance)
    write_json(output / "summary.json", report["summary"])
    state = {"adapter_evaluation_complete": report.get("adapter_evaluation_complete"),
             "input_metadata_stable": provenance.get("input_metadata_stable"),
             "adapter_invalidation": report.get("adapter_invalidation", "")}
    write_csv(output / "targets.csv", [dict(r, **state) for r in report["targets"]])
    write_csv(output / "candidates.csv", [dict(r, **state) for r in report["candidates"]])
    write_csv(output / "summary.csv", [dict(r, **state) for r in core.summary_rows(
        provenance["profile"], {report["selection_mode"]: report})])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--verifier-root", type=Path, required=True,
                        help="directory containing the unchanged recovery_verify package")
    parser.add_argument("--ground-truth", type=Path, required=True)
    parser.add_argument("--recovery-root", type=Path, required=True)
    parser.add_argument("--restore-manifest", type=Path, action="append", required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="new, non-existing directory")
    parser.add_argument("--profile", choices=("single", "dup"), required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--policy", choices=("best_include_all_stale", "latest_include_all_stale"),
                        default="best_include_all_stale")
    args = parser.parse_args(argv)
    output = Path(os.path.abspath(args.output_dir.expanduser()))
    try:
        reject_links(output)
        if output.exists():
            raise ValueError("output-dir must not already exist")
        modules, source_before = load_core(args.verifier_root)
        truth = checked_path(args.ground_truth)
        root, files, ignored, inventory = read_allowlist(args.recovery_root, args.restore_manifest, modules["evidence"])
        modules["evaluator"].validate_output_location(output, root, truth)
        source_root = checked_path(args.verifier_root)
        if output.is_relative_to(source_root) or source_root.is_relative_to(output):
            raise ValueError("output must not overlap verifier source")
        gt = modules["ground_truth"].load_ground_truth(truth)
        if gt.config.get("run_id") != "evaluation-" + args.profile:
            raise ValueError("GT run_id does not match the requested profile")
        provenance = {"adapter_version": ADAPTER_VERSION, "adapter_sha256": digest(__file__),
                      "tool": TOOL, "tool_commit": COMMIT, "profile": args.profile,
                      "run_id": args.run_id, "policy": args.policy,
                      "created_at_utc": datetime.now(timezone.utc).isoformat(),
                      "argv": sys.argv[1:] if argv is None else argv,
                      "verifier_root": str(source_root), "verifier_version": "3.0.2",
                      "source_hashes_before": source_before, "ground_truth": str(truth),
                      "ground_truth_fingerprint": gt.fingerprint, "recovery_root": str(root),
                      "output_dir": str(output), "no_persistent_measurement_cache": True}
        output.mkdir(parents=True, exist_ok=False)
        report = evaluate_pool(modules, gt, root, output, files, ignored, args.policy,
                               progress=lambda message: print(message, flush=True))
        changes = []
        for row in inventory["files"]:
            try:
                stable = row["stat"] == snapshot(row["path"])
            except OSError:
                stable = False
            if not stable:
                changes.append(row["relative_path"])
        source_after = {name: digest(source_root / "recovery_verify" / name) for name in CORE_PINS}
        manifests_stable = all(snapshot(r["path"]) == r["stat"] and digest(r["path"]) == r["sha256"]
                               for r in inventory["manifests"])
        gt_after = modules["ground_truth"].load_ground_truth(truth).fingerprint
        stable = not changes and source_before == source_after and manifests_stable and gt_after == gt.fingerprint
        provenance.update(source_hashes_after=source_after, source_unchanged=source_before == source_after,
                          manifest_inputs_unchanged=manifests_stable, changed_candidates=changes,
                          gt_fingerprint_after=gt_after, gt_unchanged=gt_after == gt.fingerprint,
                          input_metadata_stable=stable,
                          stability_limit="stat/source/manifest checks; no pre/post full candidate hashing")
        complete = apply_validity(report, stable)
        persist_report(output, report, inventory, provenance, modules["evaluator"])
        print(f"{'completed' if complete else 'incomplete'}: {output}")
        return 0 if complete else 2
    except (OSError, ValueError, KeyError, ImportError, UnicodeError, csv.Error) as exc:
        print(f"ERROR: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
