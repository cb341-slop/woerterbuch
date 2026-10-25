#!/usr/bin/env bash
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$PROJECT/build/Woerterbuch.app/Contents/MacOS/Woerterbuch"
INDEX="$PROJECT/test/index"
OUT="$(mktemp)"
ERR="$(mktemp)"
trap 'rm -f "$OUT" "$ERR"' EXIT

"$BIN" --index "$INDEX" --lookup schön --print >"$OUT"
rg -q '^Version: 2$' "$OUT"
rg -q '^Word: schön$' "$OUT"
rg -q '^Provider: DWDS$' "$OUT"
rg -q '^TDM reservation: 1$' "$OUT"
rg -q '^Title: schön$' "$OUT"
rg -q 'part_of_speech=adjective' "$OUT"
rg -q 'gender=feminine' "$OUT"
rg -q '^scōni$' "$OUT"
rg -q 'schauen \[cross_reference\]' "$OUT"

"$BIN" --index "$INDEX" --lookup SCHÖN --print >"$OUT"
rg -q '^Word: schön$' "$OUT"

if "$BIN" --index "$INDEX" --lookup unbekannt --print >"$OUT" 2>"$ERR"; then
    echo "expected an unknown word to fail" >&2
    exit 1
fi
rg -q 'not present in the dictionary manifest' "$ERR"

"$BIN" --index "$INDEX" --lookup fehlt --print >"$OUT"
rg -q '^Word: fehlt$' "$OUT"
rg -q 'Fetched entry for fehlt' "$OUT"

if "$BIN" --index "$INDEX" --lookup kaputt --print >"$OUT" 2>"$ERR"; then
    echo "expected a failed on-demand fetch to fail" >&2
    exit 1
fi
rg -q 'has not been downloaded' "$ERR"
rg -q 'simulated fetch failure' "$ERR"

echo "tests passed"
