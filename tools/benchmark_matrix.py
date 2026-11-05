#!/usr/bin/env python3
"""Run isolated benchmark workloads sequentially; emit dashboard-compatible JSONL."""
import argparse
import json
import platform
import subprocess
import sys
from datetime import datetime, timezone

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("executable", help="Path to the Release rohandb_benchmark executable")
parser.add_argument("--keys", type=int, default=10000)
parser.add_argument("--frames", type=int, nargs="+", default=[8, 64, 4096])
parser.add_argument("--include-every", action="store_true", help="Also force WAL per operation (slower)")
args = parser.parse_args()
for frames in args.frames:
    for order in ("sequential", "random"):
        for mode in (["flush", "every"] if args.include_every else ["flush"]):
            command = [args.executable, str(args.keys), str(frames), order, mode]
            print("Running " + " ".join(command), file=sys.stderr)
            result = json.loads(subprocess.check_output(command, text=True))
            result.update(platform=platform.platform(), architecture=platform.machine(),
                          measured_at=datetime.now(timezone.utc).isoformat(), command=command)
            print(json.dumps(result), flush=True)
