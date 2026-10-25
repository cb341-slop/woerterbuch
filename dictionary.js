"use strict";

const form = document.querySelector("#lookup-form");
const wordInput = document.querySelector("#word");
const pasteButton = document.querySelector("#paste");
const statusOutput = document.querySelector("#status");
const suggestions = document.querySelector("#suggestions");
const results = document.querySelector("#results");
const title = document.querySelector("#result-title");
const summary = document.querySelector("#result-summary");
const matchControl = document.querySelector("#match-control");
const matchSelect = document.querySelector("#match");
const recordRoot = document.querySelector("#record");

let currentMatches = [];
let suggestTimer;
let suggestRequest;
let lookupRequest;
const localIndex = window.WOERTERBUCH_INDEX || null;

async function loadStatus() {
  if (localIndex) return window.DictionaryIndex.status(localIndex);
  const response = await fetch("/api/status");
  if (!response.ok) throw new Error(`Index unavailable (${response.status})`);
  return response.json();
}

async function loadSuggestions(query, signal) {
  if (localIndex) return window.DictionaryIndex.suggest(localIndex, query);
  const response = await fetch(`/api/suggest?q=${encodeURIComponent(query)}`, { signal });
  if (!response.ok) throw new Error(`Suggestions unavailable (${response.status})`);
  return response.json();
}

async function loadLookup(query, signal) {
  if (localIndex) return window.DictionaryIndex.lookup(localIndex, query);
  const response = await fetch(`/api/lookup?q=${encodeURIComponent(query)}`, { signal });
  if (!response.ok) throw new Error(`Lookup failed (${response.status})`);
  return response.json();
}

function element(name, text, attributes = {}) {
  const node = document.createElement(name);
  if (text !== undefined && text !== null) node.textContent = String(text);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
  return node;
}

function setStatus(message, kind = "") {
  statusOutput.textContent = message;
  statusOutput.dataset.kind = kind;
}

function table(rows) {
  const result = element("table");
  const body = element("tbody");
  for (const [label, value] of rows) {
    if (value === undefined || value === null || value === "") continue;
    const row = element("tr");
    const cell = element("td");
    cell.append(value instanceof Node ? value : document.createTextNode(String(value)));
    row.append(element("th", label), cell);
    body.append(row);
  }
  result.append(body);
  return result;
}

function link(url) {
  return url ? element("a", url, { href: url, target: "_blank", rel: "noreferrer" }) : null;
}

function section(heading, content) {
  const node = element("section");
  node.append(element("h3", heading), content);
  return node;
}

function renderRecord(match) {
  const record = match.record;
  recordRoot.replaceChildren();
  title.textContent = match.article_title || match.word;
  summary.textContent = `${match.word} · ${match.matched_terms.map((item) => item.term).join(", ")}`;
  if (!record) {
    recordRoot.append(element("p", `The indexed entry “${match.word}” is not available yet.`));
    return;
  }

  const query = record.query || {};
  const source = record.source || {};
  const article = record.article || {};
  title.textContent = article.title || match.article_title || match.word;

  recordRoot.append(section("Query", table([
    ["Schema version", record.schema_version],
    ["Word", query.word],
    ["Requested URL", link(query.requested_url)],
    ["Match type", query.match_type],
    ["Matched headword", query.matched_headword_index ?? "none"],
  ])));

  const sourceDetails = element("details");
  sourceDetails.append(element("summary", "Source and provenance"), table([
    ["Provider", source.provider],
    ["Collection", source.collection],
    ["Attribution", source.attribution],
    ["Canonical URL", link(source.canonical_url)],
    ["Citation", source.citation],
    ["TDM reservation", source.tdm?.reservation],
    ["TDM policy", link(source.tdm?.policy_url)],
  ]));
  recordRoot.append(sourceDetails);

  recordRoot.append(section("Article", element("p", article.text || "", { class: "entry-text" })));

  const headwords = element("div");
  for (const item of record.headwords || []) {
    const grammar = item.grammar || {};
    const node = element("section", null, { class: "headword" });
    node.append(
      element("h3", item.word),
      element("small", [grammar.label, grammar.description, grammar.part_of_speech, grammar.gender]
        .filter(Boolean).join(" · ")),
      element("p", item.text || "", { class: "entry-text" }),
    );
    headwords.append(node);
  }
  recordRoot.append(section("Headwords", headwords));

  const forms = element("ul");
  for (const item of record.mentioned_forms || []) forms.append(element("li", item));
  recordRoot.append(section("Mentioned forms", forms.childElementCount ? forms : element("p", "None")));

  const links = element("ul");
  for (const item of record.links || []) {
    const anchor = element("a", item.text, { href: item.url, target: "_blank", rel: "noreferrer" });
    const listItem = element("li");
    listItem.append(anchor, document.createTextNode(` — ${item.relation}`));
    links.append(listItem);
  }
  recordRoot.append(section("Cross-references", links.childElementCount ? links : element("p", "None")));
}

function showMatch(index) {
  const match = currentMatches[index];
  if (match) renderRecord(match);
}

function updateLookupURL(word) {
  const url = new URL(location.href);
  if (url.searchParams.get("word") === word) return;
  url.searchParams.set("word", word);
  try {
    history.pushState({ word }, "", url);
  } catch {
    location.search = url.search;
  }
}

async function lookup(word, updateHistory = true) {
  const query = word.trim();
  if (!query) return;
  if (updateHistory) updateLookupURL(query);
  lookupRequest?.abort();
  lookupRequest = new AbortController();
  setStatus(`Looking up “${query}”…`);
  results.hidden = true;
  try {
    const payload = await loadLookup(query, lookupRequest.signal);
    currentMatches = payload.matches || [];
    if (payload.status === "not_found") {
      setStatus(`No indexed word matches “${query}”.`, "error");
      return;
    }
    matchSelect.replaceChildren();
    currentMatches.forEach((match, index) => {
      const label = `${match.word}${match.available ? "" : " (unavailable)"}`;
      matchSelect.append(element("option", label, { value: index }));
    });
    matchControl.hidden = currentMatches.length < 2;
    results.hidden = false;
    showMatch(0);
    const available = currentMatches.filter((match) => match.available).length;
    setStatus(`${currentMatches.length} match${currentMatches.length === 1 ? "" : "es"}; ${available} available.`, available ? "ok" : "error");
  } catch (error) {
    if (error.name !== "AbortError") setStatus(error.message, "error");
  }
}

async function updateSuggestions() {
  const query = wordInput.value.trim();
  if (!query) {
    suggestions.replaceChildren();
    return;
  }
  suggestRequest?.abort();
  suggestRequest = new AbortController();
  try {
    const payload = await loadSuggestions(query, suggestRequest.signal);
    const fragment = document.createDocumentFragment();
    for (const item of payload.suggestions) {
      fragment.append(element("option", item.label, { value: item.term }));
    }
    suggestions.replaceChildren(fragment);
  } catch (error) {
    if (error.name !== "AbortError") console.error(error);
  }
}

form?.addEventListener("submit", (event) => {
  event.preventDefault();
  lookup(wordInput.value);
});

pasteButton?.addEventListener("click", async () => {
  try {
    wordInput.value = (await navigator.clipboard.readText()).trim();
    await lookup(wordInput.value);
  } catch {
    setStatus("Clipboard access was denied. Paste into the search field instead.", "error");
    wordInput.focus();
  }
});

wordInput?.addEventListener("input", () => {
  clearTimeout(suggestTimer);
  suggestTimer = setTimeout(updateSuggestions, 80);
});

matchSelect.addEventListener("change", () => showMatch(Number(matchSelect.value)));

window.addEventListener("woerterbuch:select", (event) => {
  if (wordInput) wordInput.value = event.detail;
  lookup(event.detail);
});

window.addEventListener("popstate", () => {
  const word = new URLSearchParams(location.search).get("word");
  if (word) {
    if (wordInput) wordInput.value = word;
    lookup(word, false);
  } else {
    if (wordInput) wordInput.value = "";
    results.hidden = true;
    setStatus("Select a word or enter a lookup.");
  }
});

document.addEventListener("keydown", (event) => {
  if (wordInput && event.key === "/" && document.activeElement !== wordInput) {
    event.preventDefault();
    wordInput.focus();
    wordInput.select();
  }
});

async function initialize() {
  try {
    const payload = await loadStatus();
    const source = localIndex ? "local index" : "server index";
    setStatus(`${payload.entries} indexed words; ${payload.available} records available (${source}).`, "ok");
    const initialWord = new URLSearchParams(location.search).get("word");
    if (initialWord) {
      if (wordInput) wordInput.value = initialWord;
      await lookup(initialWord, false);
    }
  } catch (error) {
    setStatus(error.message, "error");
  }
}

initialize();
