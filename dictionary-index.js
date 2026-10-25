"use strict";

window.DictionaryIndex = (() => {
  function key(value) {
    return value.normalize("NFC").trim()
      .replace(/^[⁰¹²³⁴⁵⁶⁷⁸⁹]+/u, "")
      .toLocaleLowerCase("de-DE")
      .replaceAll("ß", "ss")
      .normalize("NFC");
  }

  function lowerBound(terms, target) {
    let low = 0;
    let high = terms.length;
    while (low < high) {
      const middle = (low + high) >>> 1;
      if (terms[middle][0] < target) low = middle + 1;
      else high = middle;
    }
    return low;
  }

  function status(index) {
    const entries = Object.values(index.entries);
    return {
      entries: entries.length,
      available: entries.filter((entry) => entry.available).length,
    };
  }

  function suggest(index, text, limit = 20) {
    const prefix = key(text);
    if (!prefix) return { suggestions: [] };
    const suggestions = [];
    const seen = new Set();
    for (let position = lowerBound(index.terms, prefix); position < index.terms.length; position += 1) {
      const [termKey, term, word] = index.terms[position];
      if (!termKey.startsWith(prefix)) break;
      const identity = `${term}\u0000${word}`;
      if (seen.has(identity)) continue;
      seen.add(identity);
      const entry = index.entries[word];
      let label = term === word ? term : `${term} → ${word}`;
      if (!entry.available) label += " (unavailable)";
      suggestions.push({ term, word, label, available: entry.available });
      if (suggestions.length === limit) break;
    }
    return { suggestions };
  }

  function lookup(index, text) {
    const queryKey = key(text);
    const grouped = new Map();
    for (let position = lowerBound(index.terms, queryKey); position < index.terms.length; position += 1) {
      const [termKey, term, word, kind] = index.terms[position];
      if (termKey !== queryKey) break;
      if (!grouped.has(word)) grouped.set(word, []);
      const matches = grouped.get(word);
      if (!matches.some((item) => item.term === term && item.kind === kind)) {
        matches.push({ term, kind });
      }
    }
    const matches = [...grouped].map(([word, matchedTerms]) => {
      const entry = index.entries[word];
      return {
        word,
        url: entry.url,
        available: entry.available,
        article_title: entry.article_title,
        match_type: entry.match_type,
        matched_terms: matchedTerms,
        record: entry.record,
      };
    });
    matches.sort((left, right) => Number(right.word === text) - Number(left.word === text));
    return {
      query: text,
      lookup_key: queryKey,
      status: matches.length === 0
        ? "not_found"
        : matches.some((match) => match.available) ? "ok" : "unavailable",
      matches,
    };
  }

  return { key, status, suggest, lookup };
})();
