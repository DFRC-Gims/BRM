"""Command-line entry points for pre-deletion byte recovery measurement."""
from __future__ import annotations
import argparse
from pathlib import Path
import sys
import sqlite3
import time

from . import __version__
from .evaluator import MODES, evaluate, validate_output_location, summary_rows, write_summary_csv


def parser():
    p=argparse.ArgumentParser(description='Measure last pre-deletion content recovery in bytes, preserving the five recovery classes.',allow_abbrev=False)
    p.add_argument('recovery_root',type=Path,help='one extraction directory, or a parent of profile directories with --batch')
    p.add_argument('ground_truth_dir',type=Path,help='schema-2.0 scenario results, or parent of evaluation-* directories with --batch')
    p.add_argument('--output-dir',type=Path,help='destination for one Proposed summary CSV or three HETMAN mode CSVs')
    p.add_argument('--tool',choices=('auto','proposed','hetman'),default='auto',
                   help='recovery tool; auto recognizes ~/output-Proposed and ~/output, including profile subdirectories')
    p.add_argument('--mode',choices=(*MODES,'all'),default=None,
                   help='HETMAN only: default all writes one CSV per selection mode; Proposed compares objects without suffix conditions')
    p.add_argument('--batch',action='store_true',help='evaluate every recovery profile against its evaluation-PROFILE ground truth')
    p.add_argument('--cache-db',type=Path,help='optional persistent cache outside inputs and output; default is memory only')
    p.add_argument('--no-cache',action='store_true',help='disable persistent reuse; modes still share in-memory measurements')
    p.add_argument('--quiet',action='store_true')
    p.add_argument('--version',action='version',version=f'%(prog)s {__version__}')
    return p


def _recognized_source(recovery):
    recovery=Path(recovery).expanduser().resolve()
    for name,tool in (('output-Proposed','proposed'),('output','hetman')):
        root=(Path.home()/name).resolve()
        if recovery.is_relative_to(root):
            return root,tool
    return None


def detect_tool(recovery):
    source=_recognized_source(recovery)
    if source is not None:
        return source[1]
    raise ValueError('cannot infer recovery tool; specify --tool proposed or --tool hetman')


def default_output(recovery,tool):
    recovery=Path(recovery).expanduser().resolve()
    source=_recognized_source(recovery)
    if source is not None:
        root,_=source
        return root.parent/'verification-v3'/tool/recovery.relative_to(root)
    return recovery.parent/'verification-v3'/f'{recovery.name}-{tool}'


class Progress:
    def __init__(self,prefix):
        self.prefix=prefix
        self.last=0.0
    def __call__(self,message):
        now=time.monotonic()
        if now-self.last >= 5 or message.startswith('Inventory'):
            print(f'[{self.prefix}] {message}',flush=True,file=sys.stderr)
            self.last=now


def main(argv=None):
    args=parser().parse_args(argv)
    try:
        recovery=args.recovery_root.expanduser().resolve()
        truth=args.ground_truth_dir.expanduser().resolve()
        if not recovery.is_dir():
            raise ValueError(f'recovery directory not found: {recovery}')
        if not truth.is_dir():
            raise ValueError(f'ground-truth directory not found: {truth}')
        recovery_tool=detect_tool(recovery) if args.tool=='auto' else args.tool
        if recovery_tool=='proposed':
            if args.mode is not None:
                raise ValueError('--mode is HETMAN-only; Proposed compares objects without duplicate selection modes')
            modes=('object',)
        else:
            modes=MODES if args.mode in (None,'all') else (args.mode,)
        output=args.output_dir.expanduser().resolve() if args.output_dir else default_output(recovery,recovery_tool)
        recognized=_recognized_source(recovery)
        input_root=recognized[0] if recognized else recovery
        validate_output_location(output,input_root,truth)
        cache=None if args.no_cache or args.cache_db is None else args.cache_db.expanduser().resolve()
        if cache is not None:
            for source in (input_root,truth,output):
                if cache.is_relative_to(source):
                    raise ValueError(f'cache must be outside evidence, ground truth and summary output: {cache}')
        from .ground_truth import load_ground_truth
        if args.batch:
            work=[]
            for profile in sorted(p for p in recovery.iterdir() if p.is_dir() and not p.is_symlink()):
                gt=truth/f'evaluation-{profile.name}'
                if not gt.is_dir():
                    raise ValueError(f'no matching ground truth for recovery profile {profile.name}: {gt}')
                work.append((profile,gt,output))
            if not work:
                raise ValueError(f'no recovery profile directories: {recovery}')
        else:
            work=[(recovery,truth,output)]
        comparison={mode:[] for mode in modes}
        incomplete=False
        for evidence,gt_path,destination in work:
            validate_output_location(destination,evidence,gt_path)
            gt=load_ground_truth(gt_path)
            reports=evaluate(gt,evidence,destination,modes=modes,recovery_tool=recovery_tool,cache_db=cache,
                             progress=None if args.quiet else Progress(evidence.name),write_summary=False)
            for mode,report in reports.items():
                comparison[mode].extend(summary_rows(evidence.name,{mode:report}))
                incomplete |= report['summary']['unverified_target_count'] > 0
                incomplete |= report['inventory_read_error_count'] > 0
        for mode,rows in comparison.items():
            path=write_summary_csv(output,rows,mode=mode)
            print(f'{"incomplete" if incomplete else "completed"}: {path}')
        return 2 if incomplete else 0
    except (OSError,ValueError,KeyError,sqlite3.Error) as exc:
        print(f'ERROR: {exc}',file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
