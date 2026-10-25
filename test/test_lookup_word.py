#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UPDATE = ROOT / "bin" / "update-sqlite"
LOOKUP = ROOT / "bin" / "lookup-word"


def run_lookup(
    word: str,
    database: Path,
    expected_status: int,
    *,
    environment: dict[str, str] | None = None,
) -> dict:
    variables = os.environ.copy()
    variables["LOOKUP_WORD_FETCH"] = "0"
    if environment:
        variables.update(environment)
    result = subprocess.run(
        [LOOKUP, word, database],
        check=False,
        capture_output=True,
        text=True,
        env=variables,
    )
    assert result.returncode == expected_status, result.stderr
    assert result.stderr == ""
    return json.loads(result.stdout)


with tempfile.TemporaryDirectory() as temporary:
    work = Path(temporary)
    index = work / "index"
    records = index / "parts" / "part-00000"
    records.mkdir(parents=True)
    database = work / "lookup.sqlite3"
    manifest = [
        {
            "ordinal": 0,
            "partition": 0,
            "word": "Straße",
            "url": "https://example.test/Strasse",
            "file": "parts/part-00000/strasse.json",
        },
        {
            "ordinal": 1,
            "partition": 0,
            "word": "⁴Schauer",
            "url": "https://example.test/Schauer#4",
            "file": "parts/part-00000/schauer.json",
        },
        {
            "ordinal": 2,
            "partition": 0,
            "word": "wartend",
            "url": "https://example.test/wartend",
            "file": "parts/part-00000/wartend.json",
        },
    ]
    with (index / "manifest.jsonl").open("w", encoding="utf-8") as stream:
        for item in manifest:
            stream.write(json.dumps(item, ensure_ascii=False) + "\n")
    street_record = {
        "query": {"word": "Straße", "match_type": "exact_headword"},
        "article": {"title": "Straße, …", "text": "Eine Straße hat oft einen Gehweg."},
        "headwords": [{"word": "Straße"}, {"word": "Sträßchen"}],
    }
    shower_record = {
        "query": {"word": "⁴Schauer", "match_type": "exact_headword"},
        "article": {"title": "schauen, …", "text": "Ein kurzer Artikel."},
        "headwords": [{"word": "Schauer"}],
    }
    (records / "strasse.json").write_text(
        json.dumps(street_record, ensure_ascii=False), encoding="utf-8"
    )
    (records / "schauer.json").write_text(
        json.dumps(shower_record, ensure_ascii=False), encoding="utf-8"
    )
    subprocess.run([UPDATE, index, database], check=True)
    (records / "strasse.json").unlink()
    (records / "schauer.json").unlink()

    listed = subprocess.run(
        [LOOKUP, "--list", database],
        check=True,
        capture_output=True,
        text=True,
    )
    assert listed.stderr == ""
    assert listed.stdout.splitlines() == ["Straße", "⁴Schauer", "wartend"]

    exact = run_lookup("Straße", database, 0)
    assert exact["status"] == "ok"
    assert [match["word"] for match in exact["matches"]] == ["Straße"]
    assert exact["matches"][0]["record"] == street_record
    assert exact["matches"][0]["matched_terms"] == [
        {"term": "Straße", "kind": "query"},
        {"term": "Straße", "kind": "headword"},
    ]

    folded = run_lookup("STRASSE", database, 0)
    assert folded["lookup_key"] == "strasse"
    assert folded["matches"][0]["word"] == "Straße"

    alias = run_lookup("strässchen", database, 0)
    assert alias["matches"][0]["matched_terms"] == [
        {"term": "Sträßchen", "kind": "headword"}
    ]

    homograph = run_lookup("Schauer", database, 0)
    assert homograph["matches"][0]["word"] == "⁴Schauer"

    fulltext = run_lookup("Gehweg", database, 0)
    assert fulltext["matches"][0]["word"] == "Straße"
    assert fulltext["matches"][0]["matched_terms"] == [
        {"term": "Gehweg", "kind": "article_text"}
    ]

    unavailable = run_lookup("wartend", database, 4)
    assert unavailable["status"] == "unavailable"
    assert unavailable["matches"][0]["record"] is None

    extractor = work / "fake-extract-word"
    extractor.write_text(
        """#!/usr/bin/env python3
import json
import sys
word = sys.argv[1]
json.dump({
    "query": {"word": word, "match_type": "exact_headword"},
    "article": {"title": "warten, …"},
    "headwords": [{"word": word}, {"word": "Wartung"}],
}, sys.stdout, ensure_ascii=False)
""",
        encoding="utf-8",
    )
    extractor.chmod(0o755)
    fetched = run_lookup(
        "wartend",
        database,
        0,
        environment={"LOOKUP_WORD_FETCH": "1", "EXTRACT_WORD": str(extractor)},
    )
    assert fetched["status"] == "ok"
    assert fetched["matches"][0]["record"]["article"]["title"] == "warten, …"
    assert not (records / "wartend.json").exists()

    stored = run_lookup("Wartung", database, 0)
    assert stored["matches"][0]["word"] == "wartend"

    missing = run_lookup("nichtvorhanden", database, 3)
    assert missing == {
        "query": "nichtvorhanden",
        "lookup_key": "nichtvorhanden",
        "status": "not_found",
        "matches": [],
    }

print("lookup-word tests passed")
