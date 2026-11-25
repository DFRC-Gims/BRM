"""Measure deletion-final content, retaining independent identity evidence."""
from __future__ import annotations

import collections
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3
from typing import Callable

from . import __version__
from .measurement import measure_file

MODES = ('no_suffix', 'max_suffix', 'all_candidates')
STATUSES = ('complete', 'partial_extent', 'content_mismatch', 'metadata_only', 'unresolve')
STATUS_PRIORITY = {status: index for index, status in enumerate(STATUSES)}


def validate_output_location(output: Path, recovery: Path, ground_truth: Path) -> None:
    output = output.expanduser().resolve()
    for source in (recovery, ground_truth):
        source = source.expanduser().resolve()
        if output.is_relative_to(source) or source.is_relative_to(output):
            raise ValueError(f'output must be outside and not a parent of input: {source}')


class MeasurementCache:
    """Only reuse measurements for unchanged files and exact reference recipes."""
    def __init__(self, path: Path | None):
        self.connection = None
        self.memory = {}
        self.hits = 0
        self.misses = 0
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
            self.connection = sqlite3.connect(path, timeout=60)
            self.connection.execute('CREATE TABLE IF NOT EXISTS measurements (key TEXT PRIMARY KEY, result TEXT NOT NULL)')
    def get(self, key):
        if key in self.memory:
            self.hits += 1
            return self.memory[key]
        if self.connection:
            row = self.connection.execute('SELECT result FROM measurements WHERE key=?', (key,)).fetchone()
            if row:
                self.hits += 1
                self.memory[key] = json.loads(row[0])
                return self.memory[key]
        self.misses += 1
        return None
    def put(self, key, value):
        self.memory[key] = value
        if self.connection:
            self.connection.execute('INSERT OR REPLACE INTO measurements VALUES (?,?)',
                                    (key, json.dumps(value, sort_keys=True)))
            self.connection.commit()
    def close(self):
        if self.connection:
            self.connection.close()


def _key(kind, *parts):
    return hashlib.sha256(repr((__version__, kind, parts)).encode()).hexdigest()


def _stat_key(path):
    s = path.stat()
    return (str(path.resolve()), s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)


def _reference_validated(target, cache):
    key = _key('reference', repr(target.recipe))
    digest = cache.get(key)
    if digest is None:
        h = hashlib.sha256()
        for offset in range(0, target.size, 1024 * 1024):
            chunk = target.recipe.read(offset, min(1024 * 1024, target.size-offset))
            if len(chunk) != min(1024 * 1024, target.size-offset):
                raise ValueError(f'incomplete reference recipe: {target.version_id}')
            h.update(chunk)
        digest = h.hexdigest()
        cache.put(key, digest)
    if digest != target.sha256:
        raise ValueError(f'reconstructed reference SHA-256 differs from manifest: {target.version_id}')


def _hash_unknown(path, cache):
    before = _stat_key(path)
    key = _key('file_hash', before)
    digest = cache.get(key)
    if digest is not None and before != _stat_key(path):
        digest = None
        before = _stat_key(path)
        key = _key('file_hash', before)
    if digest is None:
        h = hashlib.sha256()
        with path.open('rb') as f:
            while data := f.read(1024 * 1024):
                h.update(data)
        # Timestamps invalidate cached hashes but do not invalidate content.
        if before[:4] != _stat_key(path)[:4]:
            raise OSError(f'file changed during hashing: {path}')
        digest = h.hexdigest()
        cache.put(key, digest)
    return digest


def _measure(file, target, cache):
    try:
        before = _stat_key(file.path)
        key = _key('pair', before, target.sha256, repr(target.recipe))
    except OSError:
        # Let the measurement layer return a structured read error.
        return measure_file(file.path, target.recipe, target.sha256)
    result = cache.get(key)
    try:
        if result is not None and before != _stat_key(file.path):
            result = None
            before = _stat_key(file.path)
            key = _key('pair', before, target.sha256, repr(target.recipe))
    except OSError:
        result = None
    if result is None:
        result = measure_file(file.path, target.recipe, target.sha256)
        if not result['read_error']:
            try:
                if before == _stat_key(file.path):
                    cache.put(key, result)
            except OSError:
                pass
    return dict(result)


def _status(score):
    if score['read_error']:
        return 'unresolve'
    if score['complete']:
        return 'complete'
    if score['matched_bytes'] and score['has_content_evidence']:
        return 'partial_extent'
    if score['recovered_size'] == 0:
        return 'metadata_only'
    return 'content_mismatch'


def _candidate_order(row):
    return (STATUS_PRIORITY[row['status']],
            -int(row.get('matched_bytes') or 0), int(row.get('extra_bytes') or 0),
            row.get('duplicate_number') is not None,
            row.get('duplicate_number') or 0, row['recovery_relative_path'])


def summarize(rows):
    expected = sum(r['expected_size'] for r in rows)
    matched = sum(r['matched_bytes'] for r in rows)
    identified = sum(r['identified_matched_bytes'] for r in rows)
    complete = sum(bool(r['complete']) for r in rows)
    counts = collections.Counter(r['status'] for r in rows)
    errors = sum(r['read_error_count'] for r in rows)
    unverified = sum(not r['evaluation_complete'] for r in rows)
    observed = matched / expected if expected else None
    return {
        'expected_target_count': len(rows), 'expected_bytes': expected, 'matched_bytes': matched,
        'identified_matched_bytes': identified,
        'byte_recovery_rate': observed if not unverified else None,
        'observed_byte_recovery_rate': observed,
        'identified_byte_recovery_rate': identified / expected if expected else None,
        'mean_file_byte_recovery_rate': (sum(r['observed_byte_recovery_rate'] for r in rows)/len(rows) if rows and not unverified else None),
        'complete_file_count': complete, 'complete_file_rate': complete/len(rows) if rows else None,
        'unverified_target_count': unverified, 'read_error_count': errors,
        'zero_filled_candidate_count': sum(r['zero_filled_candidate_count'] for r in rows),
        'identity_ambiguous_target_count': sum(r['identity_basis'] in {'ambiguous_identity','hash_only'} for r in rows),
        'unique_best_candidate_count': len({r['best_recovery_relative_path'] for r in rows if r['best_recovery_relative_path']}),
        **{s: counts[s] for s in STATUSES},
    }


def _target_row(target, candidates):
    valid = [r for r in candidates if not r['read_error']]
    best = min(valid, key=_candidate_order) if valid else None
    identified = [r for r in valid if r['identity_confirmed']]
    identified_best = min(identified, key=_candidate_order) if identified else None
    errors = sum(bool(r['read_error']) for r in candidates)
    evaluation_complete = not errors or bool(best and best['complete'])
    row = {
        'object_id': target.object_id, 'case_id': target.case_id, 'scenario_type': target.scenario_type,
        'object_role': target.object_role, 'version_id': target.version_id, 'version_label': target.version_label,
        'initial_size_class': target.initial_size_class, 'size_class': target.size_class,
        'case_index': target.case_index, 'expected_relative_path': target.relative_path,
        'historical_aliases': list(target.aliases), 'termination_reason': target.termination_reason,
        'termination_event_id': target.termination_event_id,
        'termination_kind': 'overwrite' if target.termination_reason == 'overwritten' else 'delete',
        'expected_size': target.size, 'expected_sha256': target.sha256,
        'reference_validated': True, 'candidate_count': len(candidates),
        'read_error_count': errors, 'evaluation_complete': evaluation_complete,
        'zero_filled_candidate_count': sum(bool(r.get('zero_filled')) for r in candidates),
        'status': best['status'] if best else 'unresolve',
        'complete': bool(best and best['complete']),
        'matched_bytes': best['matched_bytes'] if best else 0,
        'mismatched_bytes': best['mismatched_bytes'] if best else (None if errors else 0),
        'missing_bytes': best['missing_bytes'] if best else (None if errors else target.size),
        'extra_bytes': best['extra_bytes'] if best else (None if errors else 0),
        'best_recovery_relative_path': best['recovery_relative_path'] if best else '',
        'best_recovered_sha256': best['recovered_sha256'] if best else '',
        'best_recovered_size': best['recovered_size'] if best else None,
        'identity_basis': best['identity_basis'] if best else 'no_candidate',
        'candidate_object_ids': best['candidate_object_ids'] if best else [],
        'identified_matched_bytes': identified_best['matched_bytes'] if identified_best else 0,
        'identified_complete': bool(identified_best and identified_best['complete']),
    }
    observed = best['byte_recovery_rate'] if best else 0.0
    row['observed_byte_recovery_rate'] = observed
    row['byte_recovery_rate'] = observed if evaluation_complete else None
    row['reason'] = ('no matching recovery candidate' if not candidates else
                     'no candidate could be verified' if not valid else
                     'exact final-content SHA-256 and length' if best['complete'] else
                     'same-offset byte comparison against the last pre-termination version')
    return row


def _write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = list(dict.fromkeys(k for r in rows for k in r))
    with path.open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: json.dumps(v, ensure_ascii=False) if isinstance(v,(dict,list,tuple)) else v for k,v in row.items()})


def evaluate(gt, recovery_root: Path, output_dir: Path, *, modes=None, recovery_tool='hetman',
             cache_db: Path | None = None, progress: Callable[[str], None] | None = None,
             write_summary: bool = True):
    from .evidence import inventory, select_files, assign_file
    recovery_root = Path(recovery_root).expanduser().resolve()
    output_dir = Path(output_dir).expanduser().resolve()
    if not recovery_root.is_dir():
        raise ValueError(f'recovery directory not found: {recovery_root}')
    if output_dir.is_relative_to(recovery_root) or recovery_root.is_relative_to(output_dir):
        raise ValueError('output directory overlaps recovery input')
    if cache_db is not None and Path(cache_db).expanduser().resolve().is_relative_to(recovery_root):
        raise ValueError('cache must not be created inside recovery input')
    if recovery_tool not in {'hetman', 'proposed'}:
        raise ValueError('recovery_tool must be hetman or proposed')
    allowed_modes = MODES if recovery_tool == 'hetman' else ('object',)
    modes = allowed_modes if modes is None else modes
    if not modes or any(m not in allowed_modes for m in modes) or len(set(modes)) != len(modes):
        raise ValueError(f'{recovery_tool} modes must be distinct members of {allowed_modes}')
    root_name = str(gt.config.get('dataset_root') or f"BTRFS_EVAL_{gt.config.get('run_id','')}").replace('\\','/').rstrip('/').split('/')[-1]
    files, ignored = inventory(recovery_root, root_name, parse_duplicates=recovery_tool == 'hetman')
    selected = {mode: select_files(files, mode) for mode in modes}
    selected_paths = {f.path for values in selected.values() for f in values}
    targets = {t.object_id: t for t in gt.targets}
    by_hash = collections.defaultdict(list)
    for t in gt.targets:
        by_hash[t.sha256].append(t.object_id)
    cache = MeasurementCache(Path(cache_db) if cache_db is not None else None)
    scores = collections.defaultdict(list)
    inventory_rows = list(ignored)
    try:
        if progress:
            progress(f'Inventory: {len(files)} evidence files, {len(ignored)} ignored; {len(targets)} terminal targets')
        for index, target in enumerate(gt.targets,1):
            _reference_validated(target, cache)
            if progress and (index == len(targets) or index % 100 == 0):
                progress(f'Reference validation: {index}/{len(targets)}')
        for index, file in enumerate(files,1):
            if file.path not in selected_paths:
                continue
            assignment = assign_file(file, gt)
            ids = [i for i in assignment['object_ids'] if i in targets]
            error = ''
            basis = assignment['identity_basis']
            confirmed = len(ids) == 1 and basis not in {'hash_only','ambiguous_identity'}
            if len(ids) > 1 and basis != 'hash_only':
                basis = 'ambiguous_identity'
            measured = None
            for object_id in ids:
                target = targets[object_id]
                score = _measure(file,target,cache)
                if not score['read_error'] and measured is None:
                    measured = dict(score)
                score.update(object_id=object_id, case_id=target.case_id,
                    recovery_relative_path=file.relative_path, duplicate_number=file.duplicate_number,
                    identity_basis=basis, identity_confirmed=confirmed, candidate_object_ids=list(ids),
                    status=_status(score))
                scores[object_id].append(score)
            # A known copy/old alias must not hide exact final content belonging
            # to another target. Reuse the full-file hash already measured above.
            digest = measured['recovered_sha256'] if measured is not None else None
            if not ids:
                try:
                    digest = _hash_unknown(file.path,cache)
                except OSError as exc:
                    error = str(exc)
            hash_ids = by_hash.get(digest, [])
            for object_id in hash_ids:
                if object_id in ids:
                    continue
                target = targets[object_id]
                if measured is not None:
                    if measured['recovered_size'] != target.size:
                        continue
                    # All target recipes were validated above. A stable read's
                    # full hash and exact length establish these content counts.
                    score = dict(measured, expected_size=target.size, expected_sha256=target.sha256,
                                 matched_bytes=target.size, mismatched_bytes=0, missing_bytes=0,
                                 extra_bytes=0, complete=True, byte_recovery_rate=1.0,
                                 has_content_evidence=True, reference_validated=True)
                else:
                    score = _measure(file,target,cache)
                    if not score['read_error'] and not score['complete']:
                        # The unknown file changed after hashing; the old hash
                        # no longer provides an attribution for partial bytes.
                        error = 'file changed between content hashing and verification'
                        continue
                score.update(object_id=object_id, case_id=target.case_id,
                    recovery_relative_path=file.relative_path, duplicate_number=file.duplicate_number,
                    identity_basis='hash_only', identity_confirmed=False, candidate_object_ids=list(hash_ids),
                    status=_status(score))
                scores[object_id].append(score)
            inventory_rows.append({'recovery_relative_path':file.relative_path,'normalized_path':file.normalized_path,
                'comparison_name':file.comparison_name,'duplicate_number':file.duplicate_number,
                'candidate_object_ids':list(ids),'hash_content_object_ids':list(hash_ids),'identity_basis':basis,
                'inventory_status':'matched' if ids or hash_ids else 'unmatched', 'read_error':error,
                'hash_unavailable': bool(error) or bool(ids and measured is None)})
            if progress:
                progress(f'Evidence: {index}/{len(files)}; measurements reused={cache.hits}, computed={cache.misses}')
        reports = {}
        generated = datetime.now(timezone.utc).isoformat()
        for mode in modes:
            pool = {f.relative_path for f in selected[mode]}
            candidate_rows = [dict(r,selection_mode=mode) for values in scores.values() for r in values if r['recovery_relative_path'] in pool]
            rows = [_target_row(t,[r for r in scores[t.object_id] if r['recovery_relative_path'] in pool]) for t in gt.targets]
            unknown_errors = sum(bool(r.get('read_error')) and r.get('recovery_relative_path') in pool
                                 for r in inventory_rows)
            hash_unavailable = sum(bool(r.get('hash_unavailable', r.get('read_error'))) and r.get('recovery_relative_path') in pool
                                   for r in inventory_rows)
            usage = collections.Counter(r['best_recovery_relative_path'] for r in rows if r['best_recovery_relative_path'])
            for r in rows:
                r['selection_mode'] = mode
                r['shared_best_candidate'] = usage[r['best_recovery_relative_path']] > 1
                r['unknown_candidate_read_error_count'] = hash_unavailable
                if hash_unavailable and not r['complete']:
                    r['evaluation_complete'] = False
                    r['byte_recovery_rate'] = None
            summary = summarize(rows)
            groups = collections.defaultdict(list)
            for row in rows:
                groups[(row['scenario_type'],row['object_role'],row['termination_kind'])].append(row)
            grouped = [dict(scenario_type=k[0],object_role=k[1],termination_kind=k[2],**summarize(v)) for k,v in sorted(groups.items())]
            reports[mode] = {
                'program_version': __version__, 'recovery_tool':recovery_tool,
                'evaluation_unit':'object_last_content_before_termination',
                'generated_at_utc':generated,'selection_mode':mode,'run_id':gt.config.get('run_id'),
                'ground_truth_fingerprint':gt.fingerprint,'recovery_root':str(recovery_root),
                'input_file_count':len(files),'selected_file_count':len(selected[mode]),
                'ignored_file_count':len(ignored),'summary':summary,
                'inventory_read_error_count':unknown_errors,
                'summary_by_termination':{kind:summarize([r for r in rows if r['termination_kind']==kind]) for kind in ('delete','overwrite')},
                'summary_by_group':grouped,'targets':rows,'candidates':candidate_rows,
                'policy':{
                    'target':'last version before delete, or before remove-destination overwrite; one target per object',
                    'byte_rate':'matching bytes at identical logical offsets / expected final size; best class, then matched bytes, one file per target',
                    'complete':'expected SHA-256 and exact length both match',
                    'duplicates':'complete > partial_extent > content_mismatch > metadata_only > unresolve; same-class tie by matched bytes then fewer extras; no merging',
                    'identity':'content availability and namespace-based object attribution are separate; hash never proves object identity',
                    'history':'extracted filenames, numeric suffixes and matching content do not prove event order or timestamps',
                    'partial':'byte equality is positional correctness; it does not establish Btrfs extent provenance',
                    'zero_filled':'nonempty all-zero input is diagnosed separately from an empty metadata stub',
                    'read_errors':'primary rates are unavailable when reads leave selection uncertain; observed rates use currently verified candidates and may rise or fall after errors are resolved',
                },
            }
            if recovery_tool == 'proposed':
                from .proposed_audit import build_variant_audit
                reports[mode]['proposed_variant_pairs'] = build_variant_audit(
                    selected[mode], inventory_rows, candidate_rows, rows, _candidate_order)
        if write_summary:
            for mode, report in reports.items():
                write_summary_csv(output_dir, summary_rows(recovery_root.name, {mode:report}), mode=mode)
        return reports
    finally:
        cache.close()


def summary_rows(profile, reports):
    """One compact table preserves the five classes and the byte denominator."""
    rows = []
    for mode, report in reports.items():
        proposed = report['recovery_tool'] == 'proposed'
        audit = report.get('proposed_variant_pairs', [])
        for group in [dict(scenario_type='ALL', object_role='ALL', termination_kind='ALL',
                           **report['summary']), *report['summary_by_group']]:
            rows.append({
                'profile': profile, 'recovery_tool':report['recovery_tool'], 'selection_mode': mode,
                'scenario_type': group['scenario_type'], 'object_role': group['object_role'],
                'termination_kind': group['termination_kind'],
                'expected_file_count': group['expected_target_count'],
                **{s: group[s] for s in STATUSES},
                'expected_bytes': group['expected_bytes'], 'recovered_bytes': group['matched_bytes'],
                'byte_recovery_rate': group['byte_recovery_rate'],
                'complete_file_rate': group['complete_file_rate'],
                'mean_file_byte_recovery_rate': group['mean_file_byte_recovery_rate'],
                'observed_byte_recovery_rate': group['observed_byte_recovery_rate'],
                'identity_ambiguous_count': group['identity_ambiguous_target_count'],
                'unverified_file_count': group['unverified_target_count'],
                'read_error_count': group['read_error_count'] + report['inventory_read_error_count'],
            })
            if proposed:
                from .proposed_audit import audit_counts
                subset = audit if group['scenario_type'] == 'ALL' else [r for r in audit if all(
                    r[k] == group[k] for k in ('scenario_type', 'object_role', 'termination_kind'))]
                rows[-1].update(row_type='summary', **audit_counts(subset))
        if proposed:
            rows.extend(dict(profile=profile, recovery_tool='proposed', selection_mode=mode, **r) for r in audit)
    return rows


def write_summary_csv(output_dir, rows, *, mode=None):
    if mode not in (None, 'object', *MODES):
        raise ValueError(f'unknown summary mode: {mode}')
    suffix = f'_{mode}' if mode in MODES else ''
    path = Path(output_dir)/f'recovery_verification_summary{suffix}.csv'
    temporary = path.with_name(path.name+'.tmp')
    _write_csv(temporary, rows)
    temporary.replace(path)
    return path
