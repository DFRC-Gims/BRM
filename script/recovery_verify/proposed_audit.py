"""Explain Complete/salvaged choices without changing content evaluation."""
from __future__ import annotations

from collections import defaultdict
from pathlib import PurePosixPath
import re

from .evidence import _WRAPPER


_FILE_LABEL = re.compile(r'^(0x[0-9a-f]+-0x[0-9a-f]+-)\[(Complete|salvaged)\]-(.+)$', re.I)
_WRAPPER_LABEL = re.compile(r'-\[(Complete|salvaged)\]$', re.I)


def variant_key(relative_path):
    """Remove only generated labels, preserving namespace and physical IDs."""
    parts = list(PurePosixPath(relative_path).parts)
    labels = set()
    if match := _FILE_LABEL.fullmatch(parts[-1]):
        labels.add(match[2].lower())
        parts[-1] = match[1] + match[3]
    if len(parts) > 1 and _WRAPPER.fullmatch(parts[-2]):
        if match := _WRAPPER_LABEL.search(parts[-2]):
            labels.add(match[1].lower())
            parts[-2] = parts[-2][:match.start()]
    # Conflicting labels cannot establish a Complete/salvaged pair.
    if len(labels) != 1:
        return None, None
    return labels.pop(), PurePosixPath(*parts).as_posix()


def _valid(rows):
    return [r for r in rows if not r.get('read_error') and r.get('measured', True)]


def _best(rows, order):
    return min(_valid(rows), key=order, default=None)


def _reason(best, rows, order):
    if best is None:
        return 'no_readable_candidate'
    alternatives = [r for r in _valid(rows) if r['recovery_relative_path'] != best['recovery_relative_path']]
    if not alternatives:
        return 'only_readable_candidate'
    runner_up = min(alternatives, key=order)
    reasons = ('better_status', 'more_matched_bytes', 'fewer_extra_bytes',
               'no_numeric_suffix', 'lower_numeric_suffix', 'path_tiebreak')
    for left, right, reason in zip(order(best), order(runner_up), reasons):
        if left != right:
            return reason
    return 'equal_quality'


def _candidate_view(row):
    return {name: row.get(name) for name in (
        'recovery_relative_path', 'status', 'matched_bytes', 'recovered_size',
        'byte_recovery_rate', 'extra_bytes', 'read_error', 'identity_basis',
        'identity_confirmed', 'measured')}


def build_variant_audit(files, inventory_rows, candidates, targets, order):
    """One comparison per physical pair and namespace-attributed target.

    Hash-only content matches never create extra duplicate cases. Ambiguous
    namespace assignments are shown for each possible target, while aggregate
    pair counts use the distinct physical keys. Unattributed pairs remain visible.
    """
    groups = defaultdict(lambda: defaultdict(list))
    for file in files:
        label, key = variant_key(file.relative_path)
        if label:
            groups[key][label].append(file.relative_path)
    inventory = {r['recovery_relative_path']: r for r in inventory_rows}
    target_by_id = {r['object_id']: r for r in targets}
    scores = defaultdict(dict)
    for row in candidates:
        scores[row['object_id']][row['recovery_relative_path']] = row
    output = []
    for key, labels in sorted(groups.items()):
        if set(labels) != {'complete', 'salvaged'}:
            continue
        paths = {p for values in labels.values() for p in values}
        ids = sorted({oid for p in paths for oid in inventory[p]['candidate_object_ids']
                      if oid in target_by_id})
        for oid in ids or ['']:
            target = target_by_id.get(oid)
            object_scores = scores.get(oid, {})
            label_rows = {}
            for label, values in labels.items():
                label_rows[label] = []
                for path in sorted(values):
                    candidate = object_scores.get(path)
                    if candidate is None:
                        candidate = dict(recovery_relative_path=path, status='unresolve',
                                         measured=False, read_error=inventory[path].get('read_error') or None)
                    else:
                        candidate = dict(candidate, measured=True)
                    label_rows[label].append(candidate)
            pair_rows = [r for values in label_rows.values() for r in values]
            pair_best = _best(pair_rows, order)
            selected_path = target['best_recovery_relative_path'] if target else ''
            selected = object_scores.get(selected_path)
            selected_variant = ('none' if not selected_path else
                                next((label for label, values in labels.items() if selected_path in values), 'other'))
            row = {
                'row_type': 'proposed_variant_pair', 'variant_pair_key': key,
                'object_id': oid, 'case_id': target['case_id'] if target else '',
                'scenario_type': target['scenario_type'] if target else 'UNMATCHED',
                'object_role': target['object_role'] if target else 'UNMATCHED',
                'termination_kind': target['termination_kind'] if target else 'UNMATCHED',
                'expected_relative_path': target['expected_relative_path'] if target else '',
                'reference_size': target['expected_size'] if target else None,
                'namespace_candidate_object_ids': ids,
                'pair_identity_basis': ('ambiguous_identity' if len(ids) > 1 else
                                        inventory[min(paths)]['identity_basis'] if ids else 'unmatched'),
                'selected_variant': selected_variant, 'selected_path': selected_path,
                'selected_status': target['status'] if target else 'unresolve',
                'selected_identity_basis': target['identity_basis'] if target else 'no_candidate',
                'selection_complete': target['evaluation_complete'] if target else False,
                'selection_reason': _reason(selected, list(object_scores.values()), order) if target else 'no_namespace_target',
                'pair_best_variant': next((label for label, values in label_rows.items() if pair_best in values), 'none') if pair_best else 'none',
                'pair_best_path': pair_best['recovery_relative_path'] if pair_best else '',
                'pair_best_status': pair_best['status'] if pair_best else 'unresolve',
                'pair_selection_reason': _reason(pair_best, pair_rows, order) if target else 'no_namespace_target',
            }
            for label in ('complete', 'salvaged'):
                values = label_rows[label]
                representative = _best(values, order) or min(values, key=order)
                row[f'{label}_candidate_count'] = len(values)
                row[f'{label}_path'] = representative['recovery_relative_path']
                for field in ('status', 'matched_bytes', 'recovered_size', 'byte_recovery_rate', 'extra_bytes', 'read_error'):
                    row[f'{label}_{field}'] = representative.get(field)
                # Preserve all variants if case differences or wrappers produce
                # more than one candidate with the same extraction label.
                row[f'{label}_candidates'] = [_candidate_view(r) for r in values]
            output.append(row)
    return output


def audit_counts(rows):
    return {
        'proposed_variant_pair_count': len({r['variant_pair_key'] for r in rows}),
        'proposed_duplicate_object_count': len({r['object_id'] for r in rows if r['object_id']}),
        'proposed_variant_comparison_count': len(rows),
        **{f'proposed_selected_{label}_count': sum(r['selected_variant'] == label for r in rows)
           for label in ('complete', 'salvaged', 'other', 'none')},
    }
