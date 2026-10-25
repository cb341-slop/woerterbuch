#!/usr/bin/env python3

import json
import sqlite3
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMMAND = ROOT / "bin" / "update-sqlite"


def write_manifest(index: Path, items: list[dict]) -> None:
    with (index / "manifest.jsonl").open("w", encoding="utf-8") as stream:
        for item in items:
            stream.write(json.dumps(item, ensure_ascii=False) + "\n")


def record(word: str, title: str, headwords: list[str]) -> dict:
    return {
        "query": {"word": word, "match_type": "exact_headword"},
        "article": {"title": title, "text": f"Text über {word}."},
        "headwords": [{"word": item} for item in headwords],
    }


with tempfile.TemporaryDirectory() as temporary:
    work = Path(temporary)
    index = work / "index"
    records = index / "parts" / "part-00000"
    records.mkdir(parents=True)
    database = work / "lookup.sqlite3"
    connection = sqlite3.connect(database)
    connection.executescript(
        """
        CREATE TABLE entries (
            word TEXT PRIMARY KEY,
            lookup_key TEXT NOT NULL,
            ordinal INTEGER NOT NULL UNIQUE,
            partition INTEGER NOT NULL,
            url TEXT NOT NULL,
            record_file TEXT NOT NULL,
            available INTEGER NOT NULL,
            article_title TEXT,
            match_type TEXT
        ) WITHOUT ROWID;
        PRAGMA user_version = 1;
        """
    )
    connection.close()
    items = [
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
    write_manifest(index, items)
    (records / "strasse.json").write_text(
        json.dumps(record("Straße", "Straße, …", ["Straße", "Sträßchen"]), ensure_ascii=False),
        encoding="utf-8",
    )

    subprocess.run([COMMAND, index, database], check=True)
    connection = sqlite3.connect(database)
    assert connection.execute("PRAGMA user_version").fetchone()[0] == 3
    assert connection.execute("SELECT count(*) FROM entries").fetchone()[0] == 3
    assert connection.execute("SELECT sum(available) FROM entries").fetchone()[0] == 1
    assert connection.execute(
        "SELECT word FROM lookup WHERE lookup_key = 'strasse' ORDER BY priority"
    ).fetchall() == [("Straße",), ("Straße",)]
    assert connection.execute(
        "SELECT word FROM lookup WHERE lookup_key = 'schauer'"
    ).fetchall() == [("⁴Schauer",)]
    assert connection.execute(
        "SELECT kind FROM lookup WHERE lookup_key = 'strässchen'"
    ).fetchone() == ("headword",)
    embedded = connection.execute(
        "SELECT record_json FROM entries WHERE word = 'Straße'"
    ).fetchone()[0]
    assert json.loads(embedded)["article"]["title"] == "Straße, …"
    assert connection.execute(
        "SELECT word FROM articles_fts WHERE articles_fts MATCH '\"Straße\"'"
    ).fetchone() == ("Straße",)
    connection.close()

    (records / "schauer.json").write_text(
        json.dumps(record("⁴Schauer", "schauen, …", ["Schauer"]), ensure_ascii=False),
        encoding="utf-8",
    )
    write_manifest(index, items[:2])
    (records / "strasse.json").unlink()
    subprocess.run([COMMAND, index, database], check=True)
    subprocess.run([COMMAND, index, database], check=True)

    connection = sqlite3.connect(database)
    assert connection.execute("SELECT count(*) FROM entries").fetchone()[0] == 2
    assert connection.execute("SELECT sum(available) FROM entries").fetchone()[0] == 2
    assert connection.execute(
        "SELECT count(record_json) FROM entries"
    ).fetchone()[0] == 2
    assert connection.execute(
        "SELECT count(*) FROM entries WHERE word = 'wartend'"
    ).fetchone()[0] == 0
    assert connection.execute(
        "SELECT count(*) FROM lookup WHERE lookup_key = 'schauer'"
    ).fetchone()[0] == 2
    assert dict(connection.execute("SELECT key, value FROM metadata"))["records"] == "2"
    connection.close()

print("update-sqlite tests passed")
