"""Append a host-observed operation to the project's development record."""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--op', required=True)
    parser.add_argument('--target', required=True)
    parser.add_argument('--reason', required=True)
    parser.add_argument('--params', default='{}')
    parser.add_argument('--status', choices=['observed', 'failed', 'unrecorded'], default='observed')
    parser.add_argument('--direction', default='ds-00001')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    path = root / 'workflow.jsonl'
    rows = [json.loads(line) for line in path.read_text(encoding='utf-8-sig').splitlines() if line]
    if not any(row.get('id') == args.direction and row.get('op') == 'user.direction' for row in rows):
        raise ValueError('The operation needs an existing user direction')
    params = json.loads(args.params)
    if not isinstance(params, dict):
        raise ValueError('Operation parameters must be an object')
    env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
    commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True, env=env).strip()
    seq = max(row['seq'] for row in rows) + 1
    row = dict(schema='ue-workflow/v0', id=f'ds-{seq:05d}', seq=seq,
               time=datetime.now().astimezone().isoformat(timespec='milliseconds'), actor='agent',
               op=args.op, target=args.target, params=params, reason=args.reason,
               reason_source='agent_note', status=args.status, direction=args.direction,
               source=dict(script=Path(__file__).relative_to(root).as_posix(),
                           sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), commit=commit))
    with path.open('a', encoding='utf-8', newline='\n') as stream:
        stream.write(json.dumps(row, ensure_ascii=False) + '\n')
        stream.flush()
        os.fsync(stream.fileno())
    print(row['id'])


if __name__ == '__main__':
    main()
