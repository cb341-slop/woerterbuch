#!/usr/bin/env bash
set -euo pipefail

# Usage: test/test.sh

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$PROJECT/bin/extract_word"
OUT="$(mktemp)"
SCHEMA="$(mktemp)"
trap 'rm -f "$OUT" "$SCHEMA"' EXIT

"$BIN" --schema >"$SCHEMA"
python3 - "$SCHEMA" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    schema = json.load(stream)

assert schema["$schema"] == "https://json-schema.org/draft/2020-12/schema"
assert schema["additionalProperties"] is False
assert set(schema["required"]) == set(schema["properties"])
assert schema["properties"]["schema_version"] == {"const": 2}
assert schema["properties"]["headwords"]["items"]["required"] == ["word", "grammar", "text"]
assert schema["properties"]["links"]["items"]["required"] == ["text", "url", "relation"]
PY

EXTRACT_WORD_HTML_FILE="$PROJECT/test/fixture.html" "$BIN" Angelpunkt >"$OUT"

python3 -m json.tool "$OUT" >/dev/null
python3 - "$OUT" "$SCHEMA" <<'PY'
import json
import sys
import jsonschema

with open(sys.argv[1], encoding="utf-8") as stream:
    item = json.load(stream)
with open(sys.argv[2], encoding="utf-8") as stream:
    schema = json.load(stream)

jsonschema.Draft202012Validator.check_schema(schema)
jsonschema.validate(item, schema)

assert item["schema_version"] == 2
assert item["query"] == {
    "word": "Angelpunkt",
    "requested_url": "https://www.dwds.de/wb/etymwb/Angelpunkt",
    "match_type": "exact_headword",
    "matched_headword_index": 3,
}
assert item["source"]["provider"] == "DWDS"
assert item["source"]["collection"] == "Etymologisches Wörterbuch"
assert item["source"]["canonical_url"].endswith("/Angelpunkt")
assert item["source"]["tdm"]["reservation"] == "1"
assert item["source"]["tdm"]["policy_url"].endswith("/tdm-policy.json")
assert "Synthetic test fixture" in item["source"]["attribution"]
assert item["source"]["citation"].startswith("Synthetic fixture")
assert item["article"]["title"] == "Angel, …"
assert len(item["article"]["text"]) > 500
assert [x["word"] for x in item["headwords"]] == ["Angel", "angeln", "Angler", "Angelpunkt"]
assert item["headwords"][0]["grammar"] == {
    "label": "f.",
    "description": "Femininum",
    "part_of_speech": "noun",
    "gender": "feminine",
}
assert item["headwords"][1]["grammar"]["part_of_speech"] == "verb"
assert item["headwords"][3]["text"].startswith("Angelpunkt m. ‘Drehpunkt’")
assert "zweite synthetische Teilstrecke" not in item["headwords"][0]["text"]
assert "angul" in item["mentioned_forms"]
assert any(x == {
    "text": "Anger",
    "url": "https://www.dwds.de/wb/etymwb/Anger",
    "relation": "cross_reference",
} for x in item["links"])
PY

EXTRACT_WORD_HTML_FILE="$PROJECT/test/fixture.html" "$BIN" 'schön' >"$OUT"
python3 - "$OUT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    item = json.load(stream)

assert item["query"]["word"] == "schön"
assert item["query"]["requested_url"].endswith("/sch%C3%B6n")
assert item["query"]["match_type"] == "article_redirect"
assert item["query"]["matched_headword_index"] is None
PY

if "$BIN" __definitely_missing_headword__ >"$OUT" 2>/dev/null; then
    echo "expected missing headword to fail" >&2
    exit 1
fi

echo "tests passed"
