#!/usr/bin/env python3
"""Look up a word in the generated SQLite routing index."""

from __future__ import annotations

import datetime
import json
import os
import sqlite3
import subprocess
import sys
from pathlib import Path
from typing import Any

from update_sqlite import SCHEMA_VERSION, article_text, lookup_key, record_details


ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"lookup-word: {message}")


def decode_record(payload: str, word: str) -> dict[str, Any]:
    try:
        record = json.loads(payload)
    except json.JSONDecodeError as error:
        fail(f"invalid embedded record for {word!r}: {error}")
    if not isinstance(record, dict):
        fail(f"embedded record for {word!r} is not a JSON object")
    return record


def open_database(database: Path, query_only: bool = True) -> sqlite3.Connection:
    try:
        connection = sqlite3.connect(database)
        if query_only:
            connection.execute("PRAGMA query_only = ON")
        connection.execute("PRAGMA foreign_keys = ON")
        version = connection.execute("PRAGMA user_version").fetchone()[0]
    except sqlite3.Error as error:
        fail(f"cannot open database {database}: {error}")
    if version != SCHEMA_VERSION:
        connection.close()
        fail(
            f"unsupported database schema version {version}; "
            "run bin/update-sqlite"
        )
    return connection


def list_words(database: Path) -> None:
    connection = open_database(database)
    try:
        rows = connection.execute(
            "SELECT word FROM entries ORDER BY ordinal"
        )
        for row in rows:
            print(row[0])
    except sqlite3.Error as error:
        fail(f"cannot query {database}: {error}")
    finally:
        connection.close()


def query(word: str, database: Path) -> tuple[dict[str, Any], int]:
    key = lookup_key(word)
    connection = open_database(database)
    connection.row_factory = sqlite3.Row
    try:
        rows = connection.execute(
            """
            SELECT
                terms.term,
                terms.kind,
                terms.priority,
                entries.word,
                entries.ordinal,
                entries.url,
                entries.record_file,
                entries.available,
                entries.article_title,
                entries.match_type,
                entries.record_json
            FROM terms
            JOIN entries ON entries.word = terms.word
            WHERE terms.lookup_key = ?
            ORDER BY
                CASE WHEN entries.word = ? THEN 0 ELSE 1 END,
                terms.priority,
                entries.ordinal,
                terms.term
            """,
            (key, word),
        ).fetchall()
        if not rows:
            phrase = '"' + word.replace('"', '""') + '"'
            rows = connection.execute(
                """
                SELECT
                    ? AS term,
                    'article_text' AS kind,
                    100 AS priority,
                    entries.word,
                    entries.ordinal,
                    entries.url,
                    entries.record_file,
                    entries.available,
                    entries.article_title,
                    entries.match_type,
                    entries.record_json
                FROM articles_fts
                JOIN entries ON entries.word = articles_fts.word
                WHERE articles_fts MATCH ?
                ORDER BY bm25(articles_fts), entries.ordinal
                LIMIT 25
                """,
                (word, phrase),
            ).fetchall()
    except sqlite3.Error as error:
        fail(f"cannot query {database}: {error}")
    finally:
        connection.close()

    if not rows:
        return {"query": word, "lookup_key": key, "status": "not_found", "matches": []}, 3

    matches: dict[str, dict[str, Any]] = {}
    for row in rows:
        match = matches.get(row["word"])
        if match is None:
            available = bool(row["available"])
            match = {
                "word": row["word"],
                "url": row["url"],
                "record_file": row["record_file"],
                "available": available,
                "article_title": row["article_title"],
                "match_type": row["match_type"],
                "matched_terms": [],
                "record": None,
            }
            if available:
                if row["record_json"] is None:
                    fail(
                        f"{row['word']!r} is marked available without embedded data; "
                        "run bin/update-sqlite"
                    )
                match["record"] = decode_record(row["record_json"], row["word"])
            matches[row["word"]] = match
        term = {"term": row["term"], "kind": row["kind"]}
        if term not in match["matched_terms"]:
            match["matched_terms"].append(term)

    result_matches = list(matches.values())
    any_available = any(match["available"] for match in result_matches)
    status = "ok" if any_available else "unavailable"
    return {
        "query": word,
        "lookup_key": key,
        "status": status,
        "matches": result_matches,
    }, 0 if any_available else 4


def store_record(database: Path, word: str, record: dict[str, Any]) -> None:
    query_data = record.get("query")
    record_word = query_data.get("word") if isinstance(query_data, dict) else None
    if record_word != word:
        fail(
            f"extractor returned query word {record_word!r} while fetching {word!r}"
        )
    title, match_type, headwords = record_details(record)
    payload = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
    connection = open_database(database, query_only=False)
    try:
        with connection:
            changed = connection.execute(
                """
                UPDATE entries
                SET available = 1,
                    article_title = ?,
                    match_type = ?,
                    record_json = ?
                WHERE word = ?
                """,
                (title, match_type, payload, word),
            ).rowcount
            if changed != 1:
                fail(f"cannot persist unindexed word {word!r}")
            connection.execute(
                "DELETE FROM terms WHERE word = ? AND kind = 'headword'", (word,)
            )
            for headword in dict.fromkeys(headwords):
                connection.execute(
                    "INSERT OR IGNORE INTO terms VALUES (?, ?, ?, 'headword', 10)",
                    (lookup_key(headword), headword, word),
                )
            connection.execute("DELETE FROM articles_fts WHERE word = ?", (word,))
            connection.execute(
                "INSERT INTO articles_fts(word, title, article_text) VALUES (?, ?, ?)",
                (word, title or "", article_text(record)),
            )
            available = connection.execute(
                "SELECT count(*) FROM entries WHERE record_json IS NOT NULL"
            ).fetchone()[0]
            connection.executemany(
                """
                INSERT INTO metadata(key, value) VALUES (?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value
                """,
                (
                    ("available_records", str(available)),
                    ("embedded_records", str(available)),
                    (
                        "updated_at",
                        datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    ),
                ),
            )
    except sqlite3.Error as error:
        fail(f"cannot persist {word!r} in {database}: {error}")
    finally:
        connection.close()


def fetch_unavailable(result: dict[str, Any], database: Path) -> list[dict[str, str]]:
    configured = os.environ.get("EXTRACT_WORD")
    extractor = Path(configured) if configured else ROOT / "extract_word/bin/extract_word"
    errors: list[dict[str, str]] = []
    if not extractor.is_file() or not os.access(extractor, os.X_OK):
        return [{"word": "", "error": f"extractor not executable: {extractor}"}]
    for match in result["matches"]:
        if match["available"]:
            continue
        process = subprocess.run(
            [extractor, match["word"]],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if process.returncode != 0:
            message = process.stderr.strip() or f"extractor exited {process.returncode}"
            errors.append({"word": match["word"], "error": message})
            continue
        try:
            record = json.loads(process.stdout)
        except json.JSONDecodeError as error:
            errors.append(
                {"word": match["word"], "error": f"extractor returned invalid JSON: {error}"}
            )
            continue
        if not isinstance(record, dict):
            errors.append(
                {"word": match["word"], "error": "extractor did not return a JSON object"}
            )
            continue
        store_record(database, match["word"], record)
    return errors


def main() -> None:
    if len(sys.argv) != 3:
        fail("usage: lookup_word.py WORD|--list DATABASE")
    if sys.argv[1] == "--list":
        list_words(Path(sys.argv[2]))
        return
    database = Path(sys.argv[2])
    result, status = query(sys.argv[1], database)
    if status == 4 and os.environ.get("LOOKUP_WORD_FETCH", "1") != "0":
        errors = fetch_unavailable(result, database)
        result, status = query(sys.argv[1], database)
        if errors:
            result["fetch_errors"] = errors
    json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    raise SystemExit(status)


if __name__ == "__main__":
    main()
