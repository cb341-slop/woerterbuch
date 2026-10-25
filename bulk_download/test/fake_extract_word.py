#!/usr/bin/env python3
import json
import os
import sys

word = sys.argv[1]
if word == os.environ.get("FAKE_FAIL_WORD"):
    print(f"simulated failure for {word}", file=sys.stderr)
    raise SystemExit(9)
if word == os.environ.get("FAKE_UNAVAILABLE_WORD"):
    print(f"no published article for {word}", file=sys.stderr)
    raise SystemExit(6)

json.dump({"schema_version": 2, "query": {"word": word}}, sys.stdout,
          ensure_ascii=False, sort_keys=True)
sys.stdout.write("\n")
