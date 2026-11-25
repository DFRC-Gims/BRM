"""Inventory extraction evidence and attribute exact historical aliases.

Names establish namespace evidence, never a content version. Numerical Btrfs
identifiers are retained for auditing but are not ground-truth object IDs.
"""
from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
import os
from pathlib import Path, PurePosixPath
import re
import stat


_PREFIX = re.compile(r'^(0x[0-9a-f]+)-(0x[0-9a-f]+)-(?:\[(?:Complete|salvaged)\]-)?(.+)$', re.I)
_WRAPPER = re.compile(
    r'^\[btrfs-recovered\]-r(0x[0-9a-f]+)-o(0x[0-9a-f]+)'
    r'-g(0x[0-9a-f]+)-t0x[0-9a-f]{2}'
    r'(?:-h[0-9a-f]{16}|-\[(?:Complete|salvaged)\])?$', re.I)
_DUPLICATE = re.compile(r'^(.*?)[ \t]*\(([0-9]+)\)(\.[^./]+)?$')


@dataclass(frozen=True)
class EvidenceFile:
    path: Path
    relative_path: str
    normalized_path: str
    basename: str
    comparison_name: str
    duplicate_number: int | None
    duplicate_group: str
    physical_object_key: str | None


def _duplicate_name(name: str) -> tuple[str, int | None]:
    match = _DUPLICATE.fullmatch(name)
    if match and match[1]:
        return match[1] + (match[3] or ''), int(match[2])
    return name, None


def _ignored(relative_path: str, reason: str) -> dict:
    return {'relative_path': relative_path, 'recovery_relative_path': relative_path,
            'inventory_status': 'ignored', 'reason': reason}


def _make_file(path: Path, root: Path, evidence_root: Path, root_name: str,
               parse_duplicates: bool = True) -> EvidenceFile:
    relative = path.relative_to(root)
    parts = list(path.relative_to(evidence_root).parts)
    if root_name in parts[:-1]:
        parts = parts[parts.index(root_name) + 1:]
    physical_key = None
    # A generated parent may carry the file's identity when a long original
    # name could not fit a prefixed component. It is not an original directory.
    if len(parts) > 1 and (wrapper := _WRAPPER.fullmatch(parts[-2])):
        physical_key = f'{wrapper[1].lower()}:{wrapper[2].lower()}:{wrapper[3].lower()}'
    parents = [part for part in parts[:-1] if not _WRAPPER.fullmatch(part)]
    actual_name = parts[-1]
    unnumbered, duplicate = _duplicate_name(actual_name) if parse_duplicates else (actual_name, None)
    prefix = _PREFIX.fullmatch(unnumbered)
    comparison = prefix[3] if prefix else unnumbered
    if prefix:
        physical_key = f'{prefix[1].lower()}:{prefix[2].lower()}'
    normalized = PurePosixPath(*parents, comparison).as_posix()
    # Keep actual parent, root/inode prefixes and Complete/salvaged marker in
    # duplicate grouping: different recovered objects are not extraction copies.
    group = (PurePosixPath(relative.parent.as_posix()) / unnumbered).as_posix()
    return EvidenceFile(path, relative.as_posix(), normalized, actual_name,
                        comparison, duplicate, group, physical_key)


def inventory(root: Path, dataset_root_name: str, *, parse_duplicates: bool = True) -> tuple[list[EvidenceFile], list[dict]]:
    root = Path(root).expanduser().absolute()
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f'recovery directory not found or is a symlink: {root}')
    ignored = []
    extract = root/'extract'
    evidence_root = extract if extract.is_dir() and not extract.is_symlink() else root
    if evidence_root != root:
        for entry in sorted(root.iterdir()):
            if entry != evidence_root:
                ignored.append(_ignored(entry.relative_to(root).as_posix(), 'outside selected extract directory'))
    files = []
    # walk does not follow symlinks; explicitly prune and report directory links.
    def walk_error(error):
        raise OSError(f'cannot inventory recovery input: {error}') from error
    for parent, directories, names in os.walk(evidence_root, followlinks=False, onerror=walk_error):
        for name in list(directories):
            path = Path(parent)/name
            if path.is_symlink():
                directories.remove(name)
                ignored.append(_ignored(path.relative_to(root).as_posix(), 'symlink'))
        directories.sort()
        for name in sorted(names):
            path = Path(parent)/name
            mode = path.lstat().st_mode
            relative = path.relative_to(root).as_posix()
            if stat.S_ISLNK(mode):
                ignored.append(_ignored(relative, 'symlink'))
            elif not stat.S_ISREG(mode):
                ignored.append(_ignored(relative, 'not a regular file'))
            elif name.lower().endswith('.dai'):
                ignored.append(_ignored(relative, 'HETMAN auxiliary session file'))
            else:
                files.append(_make_file(path, root, evidence_root, dataset_root_name, parse_duplicates))
    return sorted(files, key=lambda f: f.relative_path), sorted(ignored, key=lambda r: r['relative_path'])


def select_files(files: list[EvidenceFile], mode: str) -> list[EvidenceFile]:
    if mode in {'object', 'all_candidates'}:
        return list(files)
    if mode == 'no_suffix':
        return [file for file in files if file.duplicate_number is None]
    if mode != 'max_suffix':
        raise ValueError(f'unknown selection mode: {mode}')
    groups = defaultdict(list)
    for file in files:
        groups[file.duplicate_group].append(file)
    chosen = []
    for group in groups.values():
        numbered = [f for f in group if f.duplicate_number is not None]
        chosen.append(min(numbered or group, key=lambda f: (-(f.duplicate_number or 0), f.relative_path)))
    return sorted(chosen, key=lambda f: f.relative_path)


def build_alias_index(gt) -> dict:
    """Build once per loaded ground truth; include aliases of every version."""
    paths = defaultdict(set)
    names = defaultdict(set)
    cases = defaultdict(set)
    versions = gt.versions.values() if isinstance(gt.versions, dict) else gt.versions
    for row in versions:
        oid = str(row['object_id'])
        alias = str(row['relative_path']).replace('\\', '/').strip('/')
        paths[alias].add(oid)
        names[PurePosixPath(alias).name].add(oid)
        cases[oid].add(str(row['case_id']))
    for target in getattr(gt, 'targets', ()):
        cases[target.object_id].add(target.case_id)
        for value in target.aliases:
            alias = str(value).replace('\\', '/').strip('/')
            paths[alias].add(target.object_id)
            names[PurePosixPath(alias).name].add(target.object_id)
    index = {'paths': dict(paths), 'names': dict(names), 'cases': dict(cases)}
    try:
        gt._evidence_alias_index = index
    except (AttributeError, TypeError):
        pass
    return index


def assign_file(file: EvidenceFile, gt) -> dict:
    index = getattr(gt, '_evidence_alias_index', None)
    if index is None:
        index = build_alias_index(gt)
    parts = PurePosixPath(file.normalized_path).parts
    objects = set()
    basis = 'unmatched'
    # Match complete path components, longest suffix first; arbitrary recovery
    # wrappers may precede the original namespace, but substrings never match.
    for start in range(max(0, len(parts) - 1)):
        alias = PurePosixPath(*parts[start:]).as_posix()
        if alias in index['paths']:
            objects = index['paths'][alias]
            basis = 'historical_path'
            break
    if not objects:
        objects = index['names'].get(file.comparison_name, set())
        if objects:
            basis = 'historical_filename'
    if len(objects) > 1:
        basis = 'ambiguous_identity'
    return {'case_ids': sorted({case for oid in objects for case in index['cases'].get(oid, ())}),
            'object_ids': sorted(objects), 'identity_basis': basis}
