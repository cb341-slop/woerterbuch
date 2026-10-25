"use strict";

(() => {
const statusOutput = document.querySelector("#list-status");
const filterInput = document.querySelector("#word-filter");
const wordList = document.querySelector("#word-list");
const index = window.WOERTERBUCH_INDEX;
let renderGeneration = 0;
let filterTimer;
let filteredWords = [];

if (!index) {
  statusOutput.textContent = "Local word index not found. Run bin/build-web-index.";
} else {
  const words = Object.keys(index.entries);

  function render(matches) {
    const generation = ++renderGeneration;
    let position = 0;
    wordList.replaceChildren();
    function renderChunk() {
      if (generation !== renderGeneration) return;
      const fragment = document.createDocumentFragment();
      const end = Math.min(position + 400, matches.length);
      while (position < end) {
        const word = matches[position];
        const link = document.createElement("a");
        link.href = `?word=${encodeURIComponent(word)}`;
        link.dataset.word = word;
        link.textContent = word;
        const item = document.createElement("li");
        item.append(link);
        fragment.append(item);
        position += 1;
      }
      wordList.append(fragment);
      statusOutput.textContent = position === matches.length
        ? `${matches.length.toLocaleString()} word${matches.length === 1 ? "" : "s"}`
        : `Rendering ${position.toLocaleString()} of ${matches.length.toLocaleString()} words…`;
      if (position < matches.length) requestAnimationFrame(renderChunk);
    }
    requestAnimationFrame(renderChunk);
  }

  function applyFilter() {
    const query = filterInput.value.normalize("NFC").trim().toLocaleLowerCase("de-DE");
    filteredWords = query
      ? words.filter((word) => word.normalize("NFC").toLocaleLowerCase("de-DE").includes(query))
      : words;
    render(filteredWords);
  }

  filterInput.addEventListener("input", () => {
    clearTimeout(filterTimer);
    filterTimer = setTimeout(applyFilter, 75);
  });

  filterInput.addEventListener("keydown", (event) => {
    if (event.key !== "Enter") return;
    event.preventDefault();
    clearTimeout(filterTimer);
    applyFilter();
    if (filteredWords.length === 0) return;
    const query = filterInput.value.normalize("NFC").trim().toLocaleLowerCase("de-DE");
    const selected = filteredWords.find(
      (word) => word.normalize("NFC").toLocaleLowerCase("de-DE") === query
    ) ?? filteredWords[0];
    window.dispatchEvent(new CustomEvent("woerterbuch:select", { detail: selected }));
  });

  wordList.addEventListener("click", (event) => {
    const link = event.target.closest("a[data-word]");
    if (!link) return;
    event.preventDefault();
    window.dispatchEvent(new CustomEvent("woerterbuch:select", { detail: link.dataset.word }));
  });

  filteredWords = words;
  render(filteredWords);
}
})();
