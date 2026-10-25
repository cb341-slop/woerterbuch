# Wörterbuch clipboard app

This native macOS application reads a word from the clipboard, finds it in the
generated `word_index/manifest.jsonl`, decodes its schema-v2 JSON record, and
shows the complete entry in a selectable, scrollable AppKit dialog. Dismissing
the dialog quits the application.

The display includes every schema data group: query resolution, source and TDM
metadata, the article, structured headwords and grammar, mentioned forms, and
cross-reference links.

## Build and run

Requires macOS 13 or newer and the Swift toolchain included with Xcode or the
Command Line Tools.

```sh
make
make run
```

Copy a dictionary word before running the app. The app looks for `word_index`
beside the project automatically. To use an index elsewhere:

```sh
WOERTERBUCH_INDEX=/path/to/word_index make run
```

The built application is `build/Woerterbuch.app`. It is ad-hoc signed and can
also be launched from Finder, Spotlight, a keyboard shortcut, or an automation
tool. It runs as an accessory app, so it does not leave a Dock icon behind.

If the manifest contains the word but its JSON record is unavailable, the
dialog says so and includes the extractor's retained error detail when present.

## Command-line lookup and tests

The bundled executable has a non-GUI mode useful for scripts and diagnostics:

```sh
build/Woerterbuch.app/Contents/MacOS/Woerterbuch \
  --index ../word_index --lookup schön --print
make test
```
