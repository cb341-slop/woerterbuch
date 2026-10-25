# bulk_download

`bulk_download` turns `../etymwb-headwords.json` into a sharded on-disk index.
It schedules the existing `extract_word` executable through a bounded pthread
worker pool, writes one JSON document per headword, and records the complete
word-to-file mapping in `manifest.jsonl`.

## Build and run

Requirements: a C11 compiler and POSIX threads. Build `../extract_word` before
starting a live download.

```sh
make
make run
```

The default run uses eight workers and writes the repository-level
`../word_index/`:

```text
word_index/
├── index.json
├── manifest.jsonl
└── parts/
    ├── part-00000/
    ├── part-00001/
    └── ...
```

The 256 output partitions are selected with a stable hash of the UTF-8
headword. Thread count only controls concurrency, so changing `--workers` does
not move existing records. Keep `--partitions` unchanged when resuming an
index.

Useful options can be supplied through `ARGS`:

```sh
make run ARGS='--workers 4 --debug'
make run ARGS='--workers 12'
make run OUTPUT=path/to/index ARGS='--workers 4'
make run ARGS='--limit 10 --debug'
```

Run `./bin/bulk_download --help` for every option. Existing non-empty records
are skipped by default; `--force` replaces them. Each successful extractor
result is written to a temporary file and atomically renamed into place.

## Logs and failures

Operational logs go to standard error. `INFO` records report configuration,
progress every 100 entries, and the final counts. `--debug` also reports worker
lifecycle, scheduling, destination paths, and resume skips.

An extractor failure makes the bulk command exit nonzero. Its standard error
is retained beside the intended record as `<record>.json.error.log`, while no
partial JSON record is installed. A later run retries any failed or missing
record.

Exit status 6 from `extract_word` means the URL is present in the official
headword map but DWDS currently publishes no article for it. The downloader
reports that entry as `unavailable`, removes its expected-error log, and still
finishes successfully. It deliberately retries unavailable entries on later
runs in case DWDS publishes the article.

## Index files

`index.json` describes the index and partition count. Each line of
`manifest.jsonl` is a standalone JSON object containing:

- the source ordinal and stable partition;
- the original headword and mapped URL;
- the record path relative to the index root.

The manifest remains in source-map order even though downloads finish in a
different order.

## Development

```sh
make test
make clean
```

The integration test uses a local fake extractor and performs no network
requests.
