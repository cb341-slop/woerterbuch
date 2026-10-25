# extract_word

`extract_word` looks up a headword in `../etymwb-headwords.json`, fetches its
DWDS etymology page, traverses the page's HTML DOM, and writes one JSON record
to standard output. Diagnostics go to standard error, so successful output can
be redirected directly into an index or document store.

The included headword-to-URL map is sourced from
[`etymwb-headwords.json` in Zenodo record 14013687](https://zenodo.org/records/14013687?preview_file=etymwb-headwords.json).

## Build and run

Requirements: a C11 compiler, `pkg-config`, `libcurl`, and `libxml2`.

```sh
make
make run WORD=Angelpunkt
./bin/extract_word 'schön'
```

Run `make` from this directory. The map is found automatically when the binary
is invoked either here or from the repository root. Set `ETYMWB_HEADWORDS` to
use a map at another path.

Print the output contract as [JSON Schema Draft 2020-12](https://json-schema.org/draft/2020-12):

```sh
./bin/extract_word --schema
./bin/extract_word --schema > extract-word.schema.json
```

For a reproducible offline parse, set `EXTRACT_WORD_HTML_FILE` to an existing
HTML file. This only replaces the HTTP fetch; URL lookup and DOM traversal are
unchanged:

```sh
EXTRACT_WORD_HTML_FILE=test/fixture.html ./bin/extract_word Angelpunkt
```

## Output

The JSON record has this stable top-level shape:

```json
{
  "schema_version": 2,
  "query": {
    "word": "Angelpunkt",
    "requested_url": "https://www.dwds.de/wb/etymwb/Angelpunkt",
    "match_type": "exact_headword",
    "matched_headword_index": 3
  },
  "source": {
    "provider": "DWDS",
    "collection": "Etymologisches Wörterbuch",
    "attribution": "Etymologisches Wörterbuch (Wolfgang Pfeifer)",
    "canonical_url": "https://www.dwds.de/wb/etymwb/Angelpunkt",
    "citation": "…",
    "tdm": {
      "reservation": "1",
      "policy_url": "https://www.dwds.de/dwds_static/tdm-policy.json"
    }
  },
  "article": {"title": "Angel, …", "text": "…"},
  "headwords": [
    {
      "word": "Angelpunkt",
      "grammar": {
        "label": "m.",
        "description": "Maskulinum",
        "part_of_speech": "noun",
        "gender": "masculine"
      },
      "text": "Angelpunkt m. ‘Drehpunkt’ …"
    }
  ],
  "mentioned_forms": ["angul"],
  "links": [
    {
      "text": "Anger",
      "url": "https://www.dwds.de/wb/etymwb/Anger",
      "relation": "cross_reference"
    }
  ]
}
```

`query` distinguishes the requested map key from the article that DWDS returns.
An exact headword match includes its zero-based position so a consumer can show
the relevant section directly. `article.text` preserves the complete normalized
entry, while each headword's `text` contains only that headword's section.

Grammar keeps the source label and, where possible, adds controlled
`part_of_speech` and `gender` values plus the expanded tooltip description.
`mentioned_forms` intentionally remains an untyped, secondary-search field:
the page markup identifies these spans but does not reliably associate every
span with one language. `headwords`, `mentioned_forms`, and `links` preserve
document order and remove exact duplicates.

Source metadata is taken from page markup when present. This includes the
canonical URL, the page's citation text, and its text-and-data-mining reservation
and policy URL. Consumers remain responsible for checking and following the
referenced policy. Relative entry links are resolved against the requested URL
and classified as `cross_reference` links.

Schema version 2 replaces the flat version 1 fields (`word`, `url`, `title`,
`entry_text`, and `mentions`) with the nested fields above. A missing map key,
HTTP error, malformed page, or absent `.etymwb-entry` produces a nonzero exit
status and no partial JSON.

## Development

```sh
make test  # requires Python's jsonschema package; validates the HTML fixture
make clean # remove the generated binary
```
