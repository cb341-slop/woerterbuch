# Wörterbuch

A local pipeline for downloading DWDS EtymWB pages, extracting structured JSON,
building a full-text SQLite index, and browsing it from the command line, a
standalone HTML interface, or a native macOS clipboard app.

## Repository contents

| Path | Purpose |
| --- | --- |
| `extract_word/` | C/libxml2 extractor for one mapped DWDS headword |
| `bulk_download/` | Bounded C worker pool for resumable bulk extraction |
| `scripts/`, `bin/` | SQLite import, lookup, browser export, and local server commands |
| `index.html`, `*.js` | Offline browser interface source |
| `clipboard_dictionary/` | Native AppKit clipboard lookup application |
| `test/` | SQLite, lookup, and server integration tests |
| `etymwb-headwords.json` | Published DWDS headword-to-URL source map |

Downloaded articles, generated indexes, SQLite databases, compiled executables,
application bundles, logs, and saved third-party pages are intentionally not
versioned. They can be large and may contain material governed separately from
this project's source-code license.

## Prerequisites

- a C11 compiler, `make`, libcurl, libxml2, and POSIX threads;
- Python 3 with SQLite built with FTS5 support;
- macOS 13 and the Swift toolchain only for `clipboard_dictionary/`.

On macOS with MacPorts, the extractor build uses `pkg-config` to locate its C
dependencies. Other platforms can override the usual `CC`, `CFLAGS`, and
`LDLIBS` Make variables.

## Build the index

```sh
make -C extract_word
make -C bulk_download
make -C bulk_download run
bin/update-sqlite
```

`make -C bulk_download run` writes resumable JSON records to `word_index/`.
`bin/update-sqlite` embeds every completed record in SQLite and regenerates the
offline browser export. The generated directory is ignored by Git.

The bulk download accesses DWDS. Keep worker counts conservative and follow
the provider's terms and text/data-mining policy.

## HTML interface

Open [`index.html`](index.html) directly for a completely local,
offline interface. A dedicated `word-list.js` filters and renders all 24,461
links in cancellable 400-word animation-frame chunks inside an independently
scrollable pane. Selecting a word renders its record beside the list on the
same page; Enter selects the exact or first filtered match. The single filter
field replaces a separate suggestions dropdown. The pages load
`word_index/web-index.js` as a regular script, so
it works from a `file://` URL without `fetch`, a web server, or network access.

Regenerate the browser index after updating SQLite:

```sh
bin/build-web-index
```

The optional localhost server serves the same interface:

Start the local dictionary interface and open the printed URL:

```sh
bin/serve-dictionary
# Wörterbuch: http://127.0.0.1:8765
```

The legacy `dictionary.html` URL redirects into this unified page and preserves
its `?word=…` query.

The server binds only to localhost. Override its port or database when needed:

```sh
bin/serve-dictionary 9000 path/to/lookup.sqlite3
```

The bulk downloader writes source records under `word_index/`. The SQLite
database embeds each complete structured JSON record and provides fast exact,
alias, prefix, and article-text lookup without scanning the JSONL manifest or
reopening the source file for every request.

## Update the SQLite database

```sh
bin/update-sqlite
```

By default this reads `word_index/manifest.jsonl` and writes
`word_index/lookup.sqlite3`. A default-path update also regenerates
`word_index/web-index.js`, keeping the offline `file://` interface synchronized.
Paths can be overridden:

```sh
bin/update-sqlite path/to/index path/to/lookup.sqlite3
```

The command is transactional and safe to repeat while the downloader is being
resumed. Every available JSON document is serialized into
`entries.record_json`; rerunning the command imports newly arrived files while
preserving records already embedded by an on-demand lookup. Rows removed from
the manifest are also removed from SQLite.

`entries` contains the canonical word-to-file routing data. `terms` additionally
contains headwords extracted from available records. Its `lookup_key` is Unicode
NFC plus case folding; leading superscript homograph numbers are ignored, so a
query for `Schauer` can find `⁴Schauer`. The `lookup` view joins both tables.
`articles_fts` is an FTS5 index over article titles and text.

Exact source-key lookup:

```sh
sqlite3 -json word_index/lookup.sqlite3 \
  "SELECT * FROM entries WHERE word = 'Angelpunkt';"
```

Normalized or article-headword lookup:

```sh
sqlite3 -json word_index/lookup.sqlite3 \
  "SELECT word, record_file, available FROM lookup
   WHERE lookup_key = 'angelpunkt' ORDER BY priority, word;"
```

Prefix lookup uses a bounded range so SQLite can use the lookup-key index:

```sql
SELECT DISTINCT term, word, record_file, available
FROM lookup
WHERE lookup_key >= 'angel'
  AND lookup_key <  'angel' || char(1114111)
ORDER BY lookup_key, priority
LIMIT 20;
```

## Look up a word

```sh
bin/lookup-word Angelpunkt
bin/lookup-word Straße path/to/lookup.sqlite3
```

The command writes one JSON object to standard output. It applies the same
Unicode normalization as the updater, deduplicates source-key and headword
matches, and returns each complete record directly from SQLite under `record`.
When no exact source-key or headword matches, it searches article text and marks
those results with `kind: "article_text"`.
If a canonical word is indexed but has no embedded record yet, lookup runs
`extract_word`, saves the result in SQLite, derives its headword aliases, and
returns it. Set `EXTRACT_WORD` to override the extractor executable, or set
`LOOKUP_WORD_FETCH=0` to disable on-demand fetching.

The `status` field and process exit status distinguish outcomes:

- `ok` / exit 0: at least one matching JSON record is available;
- `not_found` / exit 3: no indexed term matches;
- `unavailable` / exit 4: the record is still pending and on-demand fetching
  failed or was disabled; details appear in `fetch_errors` when applicable.

Diagnostics go to standard error, so stdout can be piped directly to `jq`.

List every canonical word in source-map order, one per line:

```sh
bin/lookup-word --list
bin/lookup-word --list path/to/lookup.sqlite3
```

## Development

```sh
python3 test/test_update_sqlite.py  # updater integration test
python3 test/test_lookup_word.py    # lookup integration test
python3 test/test_dictionary_server.py
make -C extract_word test
make -C bulk_download test
make -C clipboard_dictionary test  # macOS only
```

## Data source

The checked-in headword map is the DWDS dataset
[Etymologisches Wörterbuch des Deutschen: Stichwortliste](https://zenodo.org/records/14013687)
(DOI `10.5281/zenodo.14013687`). It contains article names and their DWDS URLs,
not the article bodies. Extracted article content is generated locally and is
excluded from this repository. Each generated record retains its source URL,
attribution, citation text, and TDM metadata.

## License

The source code in this repository is available under the [MIT License](LICENSE).
The DWDS headword dataset and content reached through its URLs remain subject
to their respective source terms and are not relicensed by this project.
