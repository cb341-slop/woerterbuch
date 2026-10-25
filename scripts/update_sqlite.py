#!/usr/bin/env python3
"""Synchronize a bulk_download JSON index into a SQLite lookup database."""

from __future__ import annotations

import datetime
import json
import sqlite3
import sys
import unicodedata
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 3
SUPERSCRIPT_DIGITS = "⁰¹²³⁴⁵⁶⁷⁸⁹"


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"update-sqlite: {message}")


def lookup_key(value: str) -> str:
    value = unicodedata.normalize("NFC", value).strip()
    value = value.lstrip(SUPERSCRIPT_DIGITS)
    return unicodedata.normalize("NFC", value.casefold())


def required(item: dict[str, Any], key: str, expected: type, line: int) -> Any:
    value = item.get(key)
    if not isinstance(value, expected):
        fail(f"manifest line {line}: {key!r} must be {expected.__name__}")
    return value


def load_manifest(index_dir: Path) -> list[dict[str, Any]]:
    manifest = index_dir / "manifest.jsonl"
    entries: list[dict[str, Any]] = []
    words: set[str] = set()
    ordinals: set[int] = set()
    with manifest.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as error:
                fail(f"manifest line {line_number}: invalid JSON: {error}")
            if not isinstance(item, dict):
                fail(f"manifest line {line_number}: expected a JSON object")
            ordinal = required(item, "ordinal", int, line_number)
            partition = required(item, "partition", int, line_number)
            word = required(item, "word", str, line_number)
            url = required(item, "url", str, line_number)
            relative_file = required(item, "file", str, line_number)
            if ordinal < 0 or partition < 0 or not word or not url:
                fail(f"manifest line {line_number}: invalid empty or negative value")
            if word in words:
                fail(f"manifest line {line_number}: duplicate word {word!r}")
            if ordinal in ordinals:
                fail(f"manifest line {line_number}: duplicate ordinal {ordinal}")
            relative = Path(relative_file)
            if relative.is_absolute() or ".." in relative.parts:
                fail(f"manifest line {line_number}: unsafe record path {relative_file!r}")
            words.add(word)
            ordinals.add(ordinal)
            entries.append(
                {
                    "ordinal": ordinal,
                    "partition": partition,
                    "word": word,
                    "url": url,
                    "file": relative_file,
                }
            )
    return entries


def load_record(path: Path) -> dict[str, Any] | None:
    try:
        details = path.stat()
    except FileNotFoundError:
        return None
    if not path.is_file() or details.st_size == 0:
        return None
    try:
        with path.open(encoding="utf-8") as stream:
            record = json.load(stream)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"invalid record {path}: {error}")
    if not isinstance(record, dict):
        fail(f"invalid record {path}: expected a JSON object")
    return record


def record_details(record: dict[str, Any] | None) -> tuple[str | None, str | None, list[str]]:
    if record is None:
        return None, None, []
    article = record.get("article")
    title = article.get("title") if isinstance(article, dict) else None
    if not isinstance(title, str):
        title = None
    query = record.get("query")
    match_type = query.get("match_type") if isinstance(query, dict) else None
    if not isinstance(match_type, str):
        match_type = None
    headwords: list[str] = []
    values = record.get("headwords")
    if isinstance(values, list):
        for item in values:
            word = item.get("word") if isinstance(item, dict) else None
            if isinstance(word, str) and word:
                headwords.append(word)
    return title, match_type, headwords


def article_text(record: dict[str, Any] | None) -> str:
    if record is None:
        return ""
    article = record.get("article")
    text = article.get("text") if isinstance(article, dict) else None
    return text if isinstance(text, str) else ""


def initialize(connection: sqlite3.Connection) -> None:
    connection.executescript(
        """
        PRAGMA foreign_keys = ON;
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous = NORMAL;

        CREATE TABLE IF NOT EXISTS metadata (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        ) WITHOUT ROWID;

        CREATE TABLE IF NOT EXISTS entries (
            word TEXT PRIMARY KEY,
            lookup_key TEXT NOT NULL,
            ordinal INTEGER NOT NULL UNIQUE,
            partition INTEGER NOT NULL,
            url TEXT NOT NULL,
            record_file TEXT NOT NULL,
            available INTEGER NOT NULL CHECK (available IN (0, 1)),
            article_title TEXT,
            match_type TEXT,
            record_json TEXT
        ) WITHOUT ROWID;

        CREATE INDEX IF NOT EXISTS entries_lookup_key_idx
            ON entries(lookup_key);
        CREATE INDEX IF NOT EXISTS entries_available_idx
            ON entries(available);

        CREATE TABLE IF NOT EXISTS terms (
            lookup_key TEXT NOT NULL,
            term TEXT NOT NULL,
            word TEXT NOT NULL REFERENCES entries(word) ON DELETE CASCADE,
            kind TEXT NOT NULL CHECK (kind IN ('query', 'headword')),
            priority INTEGER NOT NULL,
            PRIMARY KEY (lookup_key, word, kind, term)
        ) WITHOUT ROWID;

        CREATE INDEX IF NOT EXISTS terms_word_idx ON terms(word);

        CREATE VIRTUAL TABLE IF NOT EXISTS articles_fts USING fts5(
            word UNINDEXED,
            title,
            article_text,
            tokenize = 'unicode61 remove_diacritics 0'
        );

        CREATE VIEW IF NOT EXISTS lookup AS
        SELECT
            terms.lookup_key,
            terms.term,
            terms.kind,
            terms.priority,
            entries.word,
            entries.url,
            entries.record_file,
            entries.available,
            entries.article_title,
            entries.match_type
        FROM terms
        JOIN entries ON entries.word = terms.word;
        """
    )
    version = connection.execute("PRAGMA user_version").fetchone()[0]
    if version not in (0, 1, 2, SCHEMA_VERSION):
        fail(f"unsupported database schema version {version}")
    columns = {
        row[1] for row in connection.execute("PRAGMA table_info(entries)")
    }
    if "record_json" not in columns:
        connection.execute("ALTER TABLE entries ADD COLUMN record_json TEXT")
    connection.execute(f"PRAGMA user_version = {SCHEMA_VERSION}")


def synchronize(index_dir: Path, database: Path) -> tuple[int, int, int]:
    entries = load_manifest(index_dir)
    database.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(database)
    try:
        initialize(connection)
        with connection:
            connection.execute("CREATE TEMP TABLE incoming_words (word TEXT PRIMARY KEY)")
            connection.executemany(
                "INSERT INTO incoming_words(word) VALUES (?)",
                ((item["word"],) for item in entries),
            )
            connection.execute(
                "DELETE FROM entries WHERE word NOT IN (SELECT word FROM incoming_words)"
            )
            connection.execute("DELETE FROM terms")
            connection.execute("DELETE FROM articles_fts")
            available_count = 0
            term_count = 0
            for item in entries:
                record_path = index_dir / item["file"]
                record = load_record(record_path)
                if record is None:
                    existing = connection.execute(
                        "SELECT record_json FROM entries WHERE word = ?",
                        (item["word"],),
                    ).fetchone()
                    if existing is not None and existing[0] is not None:
                        try:
                            record = json.loads(existing[0])
                        except json.JSONDecodeError as error:
                            fail(
                                f"invalid embedded record for {item['word']!r}: {error}"
                            )
                available = int(record is not None)
                available_count += available
                title, match_type, headwords = record_details(record)
                record_json = (
                    json.dumps(record, ensure_ascii=False, separators=(",", ":"))
                    if record is not None
                    else None
                )
                key = lookup_key(item["word"])
                connection.execute(
                    """
                    INSERT INTO entries (
                        word, lookup_key, ordinal, partition, url, record_file,
                        available, article_title, match_type, record_json
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(word) DO UPDATE SET
                        lookup_key = excluded.lookup_key,
                        ordinal = excluded.ordinal,
                        partition = excluded.partition,
                        url = excluded.url,
                        record_file = excluded.record_file,
                        available = excluded.available,
                        article_title = excluded.article_title,
                        match_type = excluded.match_type,
                        record_json = excluded.record_json
                    """,
                    (
                        item["word"], key, item["ordinal"], item["partition"],
                        item["url"], item["file"], available, title, match_type,
                        record_json,
                    ),
                )
                connection.execute(
                    "INSERT INTO terms VALUES (?, ?, ?, 'query', 0)",
                    (key, item["word"], item["word"]),
                )
                term_count += 1
                for headword in dict.fromkeys(headwords):
                    connection.execute(
                        "INSERT OR IGNORE INTO terms VALUES (?, ?, ?, 'headword', 10)",
                        (lookup_key(headword), headword, item["word"]),
                    )
                    term_count += connection.execute("SELECT changes()").fetchone()[0]
                if record is not None:
                    connection.execute(
                        "INSERT INTO articles_fts(word, title, article_text) VALUES (?, ?, ?)",
                        (item["word"], title or "", article_text(record)),
                    )
            metadata = {
                "schema_version": str(SCHEMA_VERSION),
                "source_index": str(index_dir.resolve()),
                "manifest": "manifest.jsonl",
                "records": str(len(entries)),
                "available_records": str(available_count),
                "embedded_records": str(available_count),
                "updated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            }
            connection.executemany(
                """
                INSERT INTO metadata(key, value) VALUES (?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value
                """,
                metadata.items(),
            )
        connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        return len(entries), available_count, term_count
    finally:
        connection.close()


def main() -> None:
    if len(sys.argv) != 3:
        fail("usage: update_sqlite.py INDEX_DIR DATABASE")
    index_dir = Path(sys.argv[1]).resolve()
    database = Path(sys.argv[2]).resolve()
    records, available, terms = synchronize(index_dir, database)
    print(
        f"updated {database}: records={records} available={available} terms={terms}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
