"""Reconstruct schema-2.0 byte recipes and the final version of each terminated object.

Recipes describe repeated patterns at absolute logical offsets. Loading validates
the event graph and manifest metadata, without materializing large files. The
measurement layer validates reconstructed reference SHA-256 values while streaming.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any


def _integer(value: Any, label: str, minimum: int = 0) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise ValueError(f'{label} must be an integer >= {minimum}')
    return value


def _text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f'{label} must be a nonempty string')
    return value


@dataclass(frozen=True)
class Segment:
    start: int
    end: int
    pattern: bytes
    origin: int


@dataclass(frozen=True)
class Recipe:
    size: int
    segments: tuple[Segment, ...]

    def __post_init__(self) -> None:
        _integer(self.size, 'recipe size')
        position = 0
        for segment in self.segments:
            _integer(segment.start, 'segment start')
            _integer(segment.end, 'segment end')
            _integer(segment.origin, 'segment origin')
            if segment.start != position or segment.end <= segment.start or segment.end > self.size:
                raise ValueError('Recipe segments must cover the file exactly without gaps or overlaps')
            if not isinstance(segment.pattern, bytes) or not segment.pattern:
                raise ValueError('Recipe patterns must be nonempty bytes')
            if segment.origin > segment.start:
                raise ValueError('Segment pattern origin cannot follow its start')
            position = segment.end
        if position != self.size:
            raise ValueError('Recipe segments do not cover its declared size')

    def read(self, offset: int, length: int) -> bytes:
        """Read a bounded byte range, preserving each pattern's original phase."""
        _integer(offset, 'read offset')
        _integer(length, 'read length')
        stop = min(self.size, offset + length)
        if offset >= stop:
            return b''
        parts = []
        for segment in self.segments:
            begin = max(offset, segment.start)
            end = min(stop, segment.end)
            if begin >= end:
                continue
            phase = (begin - segment.origin) % len(segment.pattern)
            count = end - begin
            block = segment.pattern * ((phase + count + len(segment.pattern) - 1) // len(segment.pattern))
            parts.append(block[phase:phase + count])
        return b''.join(parts)


@dataclass(frozen=True)
class Target:
    object_id: str
    case_id: str
    scenario_type: str
    object_role: str
    version_id: str
    version_label: str
    size: int
    sha256: str
    relative_path: str
    aliases: tuple[str, ...]
    recipe: Recipe
    termination_reason: str
    termination_event_id: str
    initial_size_class: str
    size_class: str
    case_index: int


@dataclass
class GroundTruth:
    config: dict[str, Any]
    cases: dict[str, dict[str, Any]]
    versions: dict[str, dict[str, Any]]
    objects: dict[str, dict[str, Any]]
    events: list[dict[str, Any]]
    recipes: dict[str, Recipe]
    targets: list[Target]
    fingerprint: str


def normalize_relative_path(value: Any, *, directory: bool = False) -> str:
    if directory and value in ('', '.'):
        return ''
    raw = _text(value, 'relative path')
    path = raw.replace('\\', '/')
    if '\x00' in path or path.startswith('/') or PureWindowsPath(raw).drive or '..' in path.split('/'):
        raise ValueError(f'Ground-truth paths must be relative and cannot traverse parents: {raw!r}')
    normalized = PurePosixPath(path).as_posix()
    if normalized == '.':
        if directory:
            return ''
        raise ValueError('A file path cannot be empty')
    return normalized


def _pattern(token: Any) -> bytes:
    return (_text(token, 'content token') + ' ').encode('utf-8')


def _repeated(size: int, token: str) -> Recipe:
    pattern = _pattern(token)
    return Recipe(size, (Segment(0, size, pattern, 0),) if size else ())


def _patch(recipe: Recipe, offset: int, length: int, pattern: bytes) -> Recipe:
    _integer(offset, 'modified range offset')
    _integer(length, 'modified range length', 1)
    end = offset + length
    if end > recipe.size:
        raise ValueError('Modified range extends past the file size')
    segments = []
    for s in recipe.segments:
        if s.end <= offset or s.start >= end:
            segments.append(s)
            continue
        if s.start < offset:
            segments.append(Segment(s.start, offset, s.pattern, s.origin))
        if s.end > end:
            segments.append(Segment(end, s.end, s.pattern, s.origin))
    segments.append(Segment(offset, end, pattern, offset))
    return Recipe(recipe.size, tuple(sorted(segments, key=lambda s: s.start)))


def _index(rows: list[dict[str, Any]], field: str) -> dict[str, dict[str, Any]]:
    result = {}
    for row in rows:
        key = _text(row.get(field), field)
        if key in result:
            raise ValueError(f'Duplicate {field}: {key}')
        result[key] = row
    return result


def _read_inputs(root: Path) -> tuple[dict[str, Any], dict[str, list[dict[str, Any]]], str]:
    names = ('run_config.json', 'case_manifest.jsonl', 'object_manifest.jsonl',
             'version_manifest.jsonl', 'ground_truth_events.jsonl')
    fingerprint = hashlib.sha256()
    parsed = {}
    for name in names:
        raw = (root / name).read_bytes()
        fingerprint.update(name.encode('ascii') + b'\0' + hashlib.sha256(raw).digest())
        try:
            if name.endswith('.jsonl'):
                rows = []
                for line_number, line in enumerate(raw.decode('utf-8-sig').splitlines(), 1):
                    if not line.strip():
                        continue
                    row = json.loads(line)
                    if not isinstance(row, dict):
                        raise ValueError(f'{name}:{line_number} must contain a JSON object')
                    rows.append(row)
                parsed[name] = rows
            else:
                parsed[name] = json.loads(raw.decode('utf-8-sig'))
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise ValueError(f'Invalid ground-truth file {name}: {exc}') from exc
    config = parsed.pop('run_config.json')
    if not isinstance(config, dict):
        raise ValueError('run_config.json must contain a JSON object')
    return config, parsed, fingerprint.hexdigest()


def load_ground_truth(path: Path) -> GroundTruth:
    """Load and validate the recorded object lifetimes, without hashing file payloads.

    Targets include delete victims and the incoming objects replaced by an
    overwrite. A copied object is separate from its source even when their bytes
    are identical. Filename changes contribute aliases, never extra targets.
    """
    config, rows, fingerprint = _read_inputs(Path(path))
    if str(config.get('schema_version') or '') != '2.0':
        raise ValueError('Ground truth must use schema_version 2.0')
    if config.get('status', 'completed') != 'completed':
        raise ValueError('Ground-truth run is not completed')
    cases = _index(rows['case_manifest.jsonl'], 'case_id')
    objects = _index(rows['object_manifest.jsonl'], 'object_id')
    versions = _index(rows['version_manifest.jsonl'], 'version_id')
    event_map = _index(rows['ground_truth_events.jsonl'], 'event_id')
    if not all((cases, objects, versions, event_map)):
        raise ValueError('Ground-truth manifests and event history must not be empty')
    events = sorted(event_map.values(), key=lambda e: _integer(e.get('event_sequence'), 'event_sequence', 1))
    if [e['event_sequence'] for e in events] != list(range(1, len(events) + 1)):
        raise ValueError('Event sequences must be unique and contiguous from one')
    for key, actual in (('scenario_case_count', len(cases)), ('historical_object_count', len(objects)),
                        ('expected_version_count', len(versions)), ('event_record_count', len(events))):
        if key in config and _integer(config[key], key) != actual:
            raise ValueError(f'Ground-truth {key} does not match the loaded records')

    object_versions: dict[str, list[dict[str, Any]]] = {oid: [] for oid in objects}
    for oid, obj in objects.items():
        case = cases.get(_text(obj.get('case_id'), f'{oid} case_id'))
        if case is None or obj.get('scenario_type') != case.get('scenario_type'):
            raise ValueError(f'Object {oid} has an unknown or inconsistent case')
        _text(obj.get('object_role'), f'{oid} object_role')
        _integer(obj.get('case_index'), f'{oid} case_index', 1)
        _text(obj.get('initial_size_class'), f'{oid} initial_size_class')
        for field in ('case_index', 'initial_size_class'):
            if obj[field] != case.get(field):
                raise ValueError(f'Object {oid} has inconsistent {field} for its case')
        if not isinstance(obj.get('final_live_presence'), bool):
            raise ValueError(f'Object {oid} is missing final_live_presence')
        if obj.get('final_relative_path') is not None:
            obj['final_relative_path'] = normalize_relative_path(obj['final_relative_path'])
    for cid, case in cases.items():
        members = case.get('object_ids')
        if (not isinstance(members, list) or any(not isinstance(x, str) or not x for x in members)
                or len(members) != len(set(members))):
            raise ValueError(f'Case {cid} must list unique object_ids')
        actual = {oid for oid, obj in objects.items() if obj['case_id'] == cid}
        if set(members) != actual or not actual:
            raise ValueError(f'Case {cid} object_ids do not match the object manifest')

    for vid, version in versions.items():
        obj = objects.get(_text(version.get('object_id'), f'{vid} object_id'))
        if obj is None:
            raise ValueError(f'Version {vid} refers to an unknown object')
        for field in ('case_id', 'scenario_type', 'object_role'):
            if version.get(field) != obj.get(field):
                raise ValueError(f'Version {vid} has inconsistent {field}')
        _text(version.get('version_label'), f'{vid} version_label')
        _text(version.get('size_class'), f'{vid} size_class')
        _integer(version.get('version_sequence'), f'{vid} version_sequence')
        _integer(version.get('size_bytes'), f'{vid} size_bytes')
        digest = _text(version.get('sha256'), f'{vid} sha256').lower()
        if len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError(f'Version {vid} has an invalid SHA-256')
        version['sha256'] = digest
        version['relative_path'] = normalize_relative_path(version.get('relative_path'))
        path_info = PurePosixPath(version['relative_path'])
        if version.get('file_name') != path_info.name:
            raise ValueError(f'Version {vid} filename disagrees with relative_path')
        expected_dir = normalize_relative_path(version.get('expected_directory', ''), directory=True)
        if expected_dir != ('' if path_info.parent.as_posix() == '.' else path_info.parent.as_posix()):
            raise ValueError(f'Version {vid} directory disagrees with relative_path')
        version['expected_directory'] = expected_dir
        tokens = version.get('content_tokens')
        if not isinstance(tokens, list) or not tokens or any(not isinstance(t, str) or not t for t in tokens):
            raise ValueError(f'Version {vid} must list content tokens')
        object_versions[obj['object_id']].append(version)
    for oid, history in object_versions.items():
        history.sort(key=lambda v: v['version_sequence'])
        if not history or [v['version_sequence'] for v in history] != list(range(len(history))):
            raise ValueError(f'Object {oid} version sequence is missing or duplicated')
        if objects[oid].get('initial_version_id') != history[0]['version_id']:
            raise ValueError(f'Object {oid} has an inconsistent initial_version_id')
        if objects[oid].get('final_version_id') != history[-1]['version_id']:
            raise ValueError(f'Object {oid} has an inconsistent final_version_id')

    recipes: dict[str, Recipe] = {}
    live: dict[str, str] = {}
    created: dict[str, str] = {}
    closed: dict[str, str] = {}
    closed_reasons: dict[str, str] = {}
    terminated: dict[str, tuple[dict[str, Any], dict[str, Any], str]] = {}

    def side(event: dict[str, Any], which: str) -> dict[str, Any] | None:
        vid = event.get('version_id_' + which)
        oid = event.get('object_id_' + which)
        if vid is None:
            if oid is not None:
                raise ValueError(f'{event["event_id"]}: object without a {which} version')
            for prefix in ('path_', 'size_', 'sha256_'):
                if event.get(prefix + which) is not None:
                    raise ValueError(f'{event["event_id"]}: metadata without a {which} version')
            return None
        vid = _text(vid, f'{event["event_id"]} version_id_{which}')
        oid = _text(oid, f'{event["event_id"]} object_id_{which}')
        version = versions.get(vid)
        if version is None or oid != version['object_id']:
            raise ValueError(f'{event["event_id"]}: invalid {which} version/object link')
        if event.get('case_id') != version['case_id'] or event.get('scenario_type') != version['scenario_type']:
            raise ValueError(f'{event["event_id"]}: event case disagrees with its version')
        for prefix, field in (('path_', 'relative_path'), ('size_', 'size_bytes'), ('sha256_', 'sha256')):
            value = event.get(prefix + which)
            if prefix == 'path_':
                value = normalize_relative_path(value)
                event[prefix + which] = value
            elif prefix == 'sha256_' and isinstance(value, str):
                value = value.lower()
            if value != version[field]:
                raise ValueError(f'{event["event_id"]}: {prefix}{which} differs from version {vid}')
        return version

    def close(version: dict[str, Any], event: dict[str, Any], reason: str) -> None:
        vid = version['version_id']
        if vid in closed:
            raise ValueError(f'Version {vid} closed more than once')
        closed[vid] = event['event_id']
        closed_reasons[vid] = reason

    def terminate(version: dict[str, Any], event: dict[str, Any], reason: str) -> None:
        oid = version['object_id']
        if oid in terminated:
            raise ValueError(f'Object {oid} terminated more than once')
        close(version, event, reason)
        terminated[oid] = (version, event, reason)
        del live[oid]

    for event in events:
        eid = event['event_id']
        before = side(event, 'before')
        after = side(event, 'after')
        kind = _text(event.get('event_type'), f'{eid} event_type')
        if kind not in {'create', 'modify', 'rename', 'move', 'copy', 'overwrite', 'delete'}:
            raise ValueError(f'Unsupported event type at {eid}: {kind!r}')
        if kind == 'create':
            if before is not None or after is None:
                raise ValueError(f'{eid}: create requires only an after version')
        elif before is None or live.get(before['object_id']) != before['version_id']:
            raise ValueError(f'{eid}: before version is not the current live version')
        if kind == 'delete':
            if after is not None:
                raise ValueError(f'{eid}: delete cannot have an after version')
            reason = _text(objects[before['object_id']].get('termination_reason'), 'termination_reason')
            if reason == 'overwritten':
                raise ValueError(f'{eid}: delete cannot be marked overwritten')
            terminate(before, event, reason)
            continue
        if after is None or after['version_id'] in recipes:
            raise ValueError(f'{eid}: missing or duplicate after version')
        oid = after['object_id']
        obj = objects[oid]
        if after.get('created_by_event_id') != eid:
            raise ValueError(f'{eid}: after version has the wrong creating event')
        if kind in {'create', 'copy', 'overwrite'}:
            if oid in created or after['version_sequence'] != 0:
                raise ValueError(f'{eid}: creation reuses an existing object')
            created[oid] = eid
            if kind in {'copy', 'overwrite'} and oid == before['object_id']:
                raise ValueError(f'{eid}: {kind} must create a distinct object')
        else:
            if oid != before['object_id'] or after['version_sequence'] != before['version_sequence'] + 1:
                raise ValueError(f'{eid}: mutation changes object identity or skips a version')

        if kind in {'create', 'overwrite'}:
            token = _text(obj.get('content_token'), f'{oid} content_token')
            if after['content_tokens'] != [token]:
                raise ValueError(f'{eid}: initial content tokens disagree with the object')
            recipe = _repeated(after['size_bytes'], token)
            if kind == 'overwrite':
                if before['relative_path'] != after['relative_path']:
                    raise ValueError(f'{eid}: overwrite changes the destination path')
                for field, expected in (('replaced_object_id', before['object_id']), ('replacement_object_id', oid)):
                    if field in event and event[field] != expected:
                        raise ValueError(f'{eid}: inconsistent {field}')
                terminate(before, event, 'overwritten')
        elif kind in {'copy', 'rename', 'move'}:
            if before['sha256'] != after['sha256'] or before['size_bytes'] != after['size_bytes']:
                raise ValueError(f'{eid}: {kind} unexpectedly changes declared content')
            if before['content_tokens'] != after['content_tokens']:
                raise ValueError(f'{eid}: {kind} unexpectedly changes content tokens')
            recipe = recipes[before['version_id']]
            if kind == 'copy':
                for field, expected in (('source_object_id', before['object_id']),
                                        ('source_version_id', before['version_id']),
                                        ('created_object_id', oid), ('created_version_id', after['version_id'])):
                    if field in event and event[field] != expected:
                        raise ValueError(f'{eid}: inconsistent {field}')
            else:
                close(before, event, 'renamed' if kind == 'rename' else 'moved')
        else:
            if before['relative_path'] != after['relative_path']:
                raise ValueError(f'{eid}: modify changes the namespace')
            subtype = event.get('event_subtype')
            if subtype == 'content_range':
                if before['size_bytes'] != after['size_bytes']:
                    raise ValueError(f'{eid}: range modification changes the file size')
                patches = event.get('modified_ranges')
                if not isinstance(patches, list) or not patches:
                    raise ValueError(f'{eid}: content modification has no recorded ranges')
                recipe = recipes[before['version_id']]
                for patch in patches:
                    if not isinstance(patch, dict):
                        raise ValueError(f'{eid}: invalid modified range')
                    token = _text(patch.get('content_token'), 'modified content_token')
                    pattern = _pattern(token)
                    if 'content_pattern' in patch and patch['content_pattern'] != token + ' ':
                        raise ValueError(f'{eid}: modified token and pattern disagree')
                    if token not in after['content_tokens']:
                        raise ValueError(f'{eid}: modified token is absent from the resulting version')
                    recipe = _patch(recipe, patch.get('offset'), patch.get('length'), pattern)
                close(before, event, 'modified')
            elif subtype == 'size_class_change':
                token = _text(obj.get('resized_content_token'), f'{oid} resized_content_token')
                if after['content_tokens'] != [token]:
                    raise ValueError(f'{eid}: resize content tokens disagree with the object')
                recipe = _repeated(after['size_bytes'], token)
                close(before, event, 'size_class_changed')
            else:
                raise ValueError(f'{eid}: unsupported modify subtype {subtype!r}')
        if recipe.size != after['size_bytes']:
            raise ValueError(f'{eid}: recipe size disagrees with the resulting version')
        recipes[after['version_id']] = recipe
        live[oid] = after['version_id']

    if set(recipes) != set(versions) or set(created) != set(objects):
        raise ValueError('Manifest contains versions or objects unreachable from the event graph')
    for vid, version in versions.items():
        if version.get('closed_by_event_id') != closed.get(vid):
            raise ValueError(f'Version {vid} has an inconsistent closing event')
        if version.get('closed_reason') != closed_reasons.get(vid):
            raise ValueError(f'Version {vid} has an inconsistent closing reason')
    targets = []
    for oid, obj in objects.items():
        if obj.get('created_by_event_id') != created[oid]:
            raise ValueError(f'Object {oid} has an inconsistent creating event')
        if obj['final_live_presence'] != (oid in live):
            raise ValueError(f'Object {oid} final live presence disagrees with its events')
        if oid not in terminated:
            if obj.get('terminated_by_event_id') is not None or obj.get('termination_reason') is not None:
                raise ValueError(f'Live object {oid} declares a termination')
            if obj.get('final_relative_path') != versions[live[oid]]['relative_path']:
                raise ValueError(f'Live object {oid} has an inconsistent final path')
            continue
        version, event, reason = terminated[oid]
        if obj.get('terminated_by_event_id') != event['event_id'] or obj.get('termination_reason') != reason:
            raise ValueError(f'Object {oid} termination disagrees with its event history')
        if obj.get('final_version_id') != version['version_id'] or obj.get('final_relative_path') is not None:
            raise ValueError(f'Object {oid} final version/path disagrees with its termination')
        aliases = tuple(dict.fromkeys(v['relative_path'] for v in object_versions[oid]))
        targets.append(Target(
            object_id=oid, case_id=obj['case_id'], scenario_type=obj['scenario_type'],
            object_role=obj['object_role'], version_id=version['version_id'],
            version_label=version['version_label'], size=version['size_bytes'], sha256=version['sha256'],
            relative_path=version['relative_path'], aliases=aliases, recipe=recipes[version['version_id']],
            termination_reason=reason, termination_event_id=event['event_id'],
            initial_size_class=obj['initial_size_class'], size_class=version['size_class'],
            case_index=obj['case_index'],
        ))
    if 'final_live_object_count' in config and _integer(config['final_live_object_count'], 'final_live_object_count') != len(live):
        raise ValueError('Configured final live object count disagrees with the event graph')
    targets.sort(key=lambda t: (t.case_id, t.object_id))
    return GroundTruth(config, cases, versions, objects, events, recipes, targets, fingerprint)
