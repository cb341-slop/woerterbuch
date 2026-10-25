#!/usr/bin/env python3
"""Export the SQLite dictionary as a browser-loadable JavaScript index."""

from __future__ import annotations

import json
import os
import sqlite3
import sys
from pathlib import Path

from lookup_word import decode_record, open_database
from update_sqlite import lookup_key


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: build_web_index.py DATABASE OUTPUT HEADWORD_MAP")
    database = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    headword_map = Path(sys.argv[3]).resolve()
    with headword_map.open(encoding="utf-8") as stream:
        source_entries = json.load(stream)
    if not isinstance(source_entries, dict):
        raise SystemExit(f"headword map is not a JSON object: {headword_map}")
    connection = open_database(database)
    connection.row_factory = sqlite3.Row
    try:
        rows = connection.execute(
            """
            SELECT word, url, available, article_title, match_type, record_json
            FROM entries ORDER BY ordinal
            """
        ).fetchall()
        term_rows = connection.execute(
            """
            SELECT terms.lookup_key, terms.term, terms.word, terms.kind
            FROM terms
            JOIN entries ON entries.word = terms.word
            ORDER BY terms.lookup_key, terms.priority, entries.ordinal, terms.term
            """
        ).fetchall()
    finally:
        connection.close()

    entries = {
        word: {
            "url": url,
            "available": False,
            "article_title": None,
            "match_type": None,
            "record": None,
        }
        for word, url in source_entries.items()
    }
    for row in rows:
        record = None
        if row["record_json"] is not None:
            record = decode_record(row["record_json"], row["word"])
        entries[row["word"]] = {
            "url": row["url"],
            "available": bool(row["available"]),
            "article_title": row["article_title"],
            "match_type": row["match_type"],
            "record": record,
        }
    ordinals = {word: ordinal for ordinal, word in enumerate(entries)}
    terms = {
        (lookup_key(word), word, word, "query")
        for word in entries
    }
    terms.update(tuple(row) for row in term_rows)
    ordered_terms = sorted(
        terms,
        key=lambda item: (
            item[0],
            0 if item[3] == "query" else 10,
            ordinals.get(item[2], len(ordinals)),
            item[1],
        ),
    )
    payload = {
        "entries": entries,
        "terms": [list(row) for row in ordered_terms],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp.{os.getpid()}")
    with temporary.open("w", encoding="utf-8") as stream:
        stream.write("\"use strict\";\nwindow.WOERTERBUCH_INDEX=")
        json.dump(payload, stream, ensure_ascii=False, separators=(",", ":"))
        stream.write(";\n")
    temporary.replace(output)
    available = sum(entry["available"] for entry in entries.values())
    print(
        f"wrote {output}: entries={len(entries)} available={available} "
        f"terms={len(ordered_terms)}"
    )


if __name__ == "__main__":
    main()
