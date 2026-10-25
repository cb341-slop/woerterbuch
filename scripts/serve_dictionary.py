#!/usr/bin/env python3
"""Serve the local dictionary HTML interface and SQLite lookup API."""

from __future__ import annotations

import json
import sqlite3
import sys
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from lookup_word import open_database, query
from update_sqlite import lookup_key


ROOT = Path(__file__).resolve().parents[1]
STATIC_FILES = {
    "/": (ROOT / "index.html", "text/html; charset=utf-8"),
    "/index.html": (ROOT / "index.html", "text/html; charset=utf-8"),
    "/dictionary.html": (ROOT / "dictionary.html", "text/html; charset=utf-8"),
    "/dictionary.js": (ROOT / "dictionary.js", "text/javascript; charset=utf-8"),
    "/word-list.js": (ROOT / "word-list.js", "text/javascript; charset=utf-8"),
    "/dictionary-index.js": (ROOT / "dictionary-index.js", "text/javascript; charset=utf-8"),
    "/word_index/web-index.js": (
        ROOT / "word_index/web-index.js",
        "text/javascript; charset=utf-8",
    ),
}


def status_payload(database: Path) -> dict[str, int]:
    connection = open_database(database)
    try:
        entries, available = connection.execute(
            "SELECT count(*), sum(available) FROM entries"
        ).fetchone()
    finally:
        connection.close()
    return {"entries": entries, "available": available or 0}


def suggestions_payload(text: str, database: Path) -> dict[str, list[dict[str, object]]]:
    key = lookup_key(text.strip())
    if not key:
        return {"suggestions": []}
    connection = open_database(database)
    connection.row_factory = sqlite3.Row
    try:
        rows = connection.execute(
            """
            SELECT terms.term, terms.kind, entries.word, entries.available,
                   entries.article_title, terms.lookup_key, terms.priority,
                   entries.ordinal
            FROM terms
            JOIN entries ON entries.word = terms.word
            WHERE terms.lookup_key >= ?
              AND terms.lookup_key < ? || char(1114111)
            ORDER BY terms.lookup_key = ? DESC,
                     terms.lookup_key, terms.priority, entries.ordinal
            LIMIT 60
            """,
            (key, key, key),
        ).fetchall()
    finally:
        connection.close()
    seen: set[tuple[str, str]] = set()
    suggestions = []
    for row in rows:
        identity = (row["term"], row["word"])
        if identity in seen:
            continue
        seen.add(identity)
        label = row["term"]
        if row["term"] != row["word"]:
            label += f" → {row['word']}"
        if not row["available"]:
            label += " (unavailable)"
        suggestions.append(
            {
                "term": row["term"],
                "word": row["word"],
                "label": label,
                "available": bool(row["available"]),
            }
        )
        if len(suggestions) == 20:
            break
    return {"suggestions": suggestions}


class DictionaryServer(ThreadingHTTPServer):
    database: Path


class Handler(BaseHTTPRequestHandler):
    server: DictionaryServer

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path in STATIC_FILES:
            path, content_type = STATIC_FILES[parsed.path]
            self.send_bytes(path.read_bytes(), content_type)
            return
        parameters = parse_qs(parsed.query)
        if parsed.path == "/api/status":
            self.status()
        elif parsed.path == "/api/suggest":
            self.suggest(parameters.get("q", [""])[0])
        elif parsed.path == "/api/lookup":
            self.lookup(parameters.get("q", [""])[0])
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def status(self) -> None:
        self.send_json(status_payload(self.server.database))

    def suggest(self, text: str) -> None:
        self.send_json(suggestions_payload(text, self.server.database))

    def lookup(self, word: str) -> None:
        word = word.strip()
        if not word:
            self.send_json({"error": "q must contain a word"}, HTTPStatus.BAD_REQUEST)
            return
        payload, _ = query(word, self.server.database)
        self.send_json(payload)

    def send_json(self, payload: object, status: HTTPStatus = HTTPStatus.OK) -> None:
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def send_bytes(self, data: bytes, content_type: str) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format: str, *arguments: object) -> None:
        print(f"dictionary: {format % arguments}", file=sys.stderr)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: serve_dictionary.py PORT DATABASE")
    try:
        port = int(sys.argv[1])
    except ValueError:
        raise SystemExit(f"invalid port: {sys.argv[1]}") from None
    if not 1 <= port <= 65535:
        raise SystemExit(f"invalid port: {port}")
    database = Path(sys.argv[2]).resolve()
    if not database.is_file():
        raise SystemExit(f"database not found: {database}")
    server = DictionaryServer(("127.0.0.1", port), Handler)
    server.database = database
    print(f"Wörterbuch: http://127.0.0.1:{port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
