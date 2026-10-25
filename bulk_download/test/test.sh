#!/usr/bin/env bash
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$PROJECT/bin/bulk_download"
FIXTURE="$PROJECT/test/fixture-map.json"
FAILURE_FIXTURE="$PROJECT/test/failure-map.json"
EXTRACTOR="$PROJECT/test/fake_extract_word.py"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

"$BIN" \
    --map "$FIXTURE" \
    --extractor "$EXTRACTOR" \
    --output "$WORK/index" \
    --workers 3 \
    --partitions 4 \
    --debug 2>"$WORK/first.log"

python3 - "$WORK/index" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
metadata = json.loads((root / "index.json").read_text(encoding="utf-8"))
assert metadata["schema_version"] == 1
assert metadata["records"] == 4
assert metadata["partitions"] == 4

lines = (root / "manifest.jsonl").read_text(encoding="utf-8").splitlines()
manifest = [json.loads(line) for line in lines]
assert [item["word"] for item in manifest] == ["Aal", "schön", "Straße", "Zug"]
assert [item["ordinal"] for item in manifest] == list(range(4))
for item in manifest:
    assert item["file"].startswith(f"parts/part-{item['partition']:05d}/")
    record = json.loads((root / item["file"]).read_text(encoding="utf-8"))
    assert record["query"]["word"] == item["word"]
PY

grep -q 'level=DEBUG worker=.*event=worker_started' "$WORK/first.log"
grep -q 'event=complete total=4 downloaded=4 skipped=0 unavailable=0 failed=0' "$WORK/first.log"

"$BIN" \
    --map "$FIXTURE" \
    --extractor "$EXTRACTOR" \
    --output "$WORK/index" \
    --workers 2 \
    --partitions 4 \
    --debug 2>"$WORK/resume.log"
grep -q 'event=complete total=4 downloaded=0 skipped=4 unavailable=0 failed=0' "$WORK/resume.log"

"$BIN" \
    --map "$FIXTURE" \
    --extractor "$EXTRACTOR" \
    --output "$WORK/index" \
    --workers 2 \
    --partitions 4 \
    --force 2>"$WORK/force.log"
grep -q 'event=complete total=4 downloaded=4 skipped=0 unavailable=0 failed=0' "$WORK/force.log"

if FAKE_FAIL_WORD=kaputt "$BIN" \
    --map "$FAILURE_FIXTURE" \
    --extractor "$EXTRACTOR" \
    --output "$WORK/failure-index" \
    --workers 2 \
    --partitions 2 2>"$WORK/failure.log"; then
    echo "expected extractor failure to make bulk_download fail" >&2
    exit 1
fi

grep -q 'event=extract_failed.*word="kaputt".*exit=9' "$WORK/failure.log"
grep -q 'event=complete total=2 downloaded=1 skipped=0 unavailable=0 failed=1' "$WORK/failure.log"
python3 - "$WORK/failure-index" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
manifest = [json.loads(line) for line in
            (root / "manifest.jsonl").read_text(encoding="utf-8").splitlines()]
failed = next(item for item in manifest if item["word"] == "kaputt")
record = root / failed["file"]
assert not record.exists()
error_log = pathlib.Path(str(record) + ".error.log")
assert "simulated failure for kaputt" in error_log.read_text(encoding="utf-8")
PY

FAKE_UNAVAILABLE_WORD=kaputt "$BIN" \
    --map "$FAILURE_FIXTURE" \
    --extractor "$EXTRACTOR" \
    --output "$WORK/unavailable-index" \
    --workers 2 \
    --partitions 2 2>"$WORK/unavailable.log"

grep -q 'event=unavailable.*word="kaputt"' "$WORK/unavailable.log"
grep -q 'event=complete total=2 downloaded=1 skipped=0 unavailable=1 failed=0' "$WORK/unavailable.log"
python3 - "$WORK/unavailable-index" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
manifest = [json.loads(line) for line in
            (root / "manifest.jsonl").read_text(encoding="utf-8").splitlines()]
unavailable = next(item for item in manifest if item["word"] == "kaputt")
record = root / unavailable["file"]
assert not record.exists()
assert not pathlib.Path(str(record) + ".error.log").exists()
PY

echo "tests passed"
