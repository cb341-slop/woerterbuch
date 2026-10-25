#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from lookup_word import query  # noqa: E402
from serve_dictionary import status_payload, suggestions_payload  # noqa: E402


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
            "word": "wartend",
            "url": "https://example.test/wartend",
            "file": "parts/part-00000/wartend.json",
        },
    ]
    with (index / "manifest.jsonl").open("w", encoding="utf-8") as stream:
        for item in manifest:
            stream.write(json.dumps(item, ensure_ascii=False) + "\n")
    record = {
        "schema_version": 2,
        "query": {
            "word": "Straße",
            "requested_url": "https://example.test/Strasse",
            "match_type": "exact_headword",
            "matched_headword_index": 0,
        },
        "source": {
            "provider": "DWDS",
            "collection": "Etymologisches Wörterbuch",
            "canonical_url": "https://example.test/Strasse",
        },
        "article": {"title": "Straße", "text": "Straße f. Verkehrsweg."},
        "headwords": [
            {
                "word": "Straße",
                "grammar": {"label": "f.", "part_of_speech": "noun"},
                "text": "Straße f. Verkehrsweg.",
            },
            {
                "word": "Sträßchen",
                "grammar": {"label": "n.", "part_of_speech": "noun"},
                "text": "Sträßchen n. kleine Straße.",
            },
        ],
        "mentioned_forms": ["strāza"],
        "links": [],
    }
    (records / "strasse.json").write_text(
        json.dumps(record, ensure_ascii=False), encoding="utf-8"
    )
    subprocess.run([ROOT / "bin/update-sqlite", index, database], check=True)

    html = (ROOT / "dictionary.html").read_text(encoding="utf-8")
    assert "<title>Wörterbuch</title>" in html
    assert 'rel="icon"' in html
    assert 'id="lookup-form"' in html

    status = status_payload(database)
    assert status == {"entries": 2, "available": 1}

    suggestions = suggestions_payload("str", database)
    assert suggestions["suggestions"][0]["term"] == "Straße"
    assert suggestions["suggestions"][0]["available"] is True
    assert suggestions_payload("", database) == {"suggestions": []}

    exact, exact_status = query("STRASSE", database)
    assert exact_status == 0
    assert exact["status"] == "ok"
    assert exact["matches"][0]["record"] == record

    alias, alias_status = query("strässchen", database)
    assert alias_status == 0
    assert alias["matches"][0]["word"] == "Straße"

    unavailable, unavailable_status = query("wartend", database)
    assert unavailable_status == 4
    assert unavailable["status"] == "unavailable"

    missing, missing_status = query("unbekannt", database)
    assert missing_status == 3
    assert missing["status"] == "not_found"

print("dictionary server tests passed")
