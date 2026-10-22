"use strict";

(() => {
  const icons = {
    copy: '<rect x="8" y="8" width="12" height="12" rx="2"/><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/>',
    copied: '<path d="m5 12 4 4L19 6"/>',
    failed: '<path d="m6 6 12 12M18 6 6 18"/>',
  };
  const labels = { copy: "Copy", copied: "Copied!", failed: "Copy failed" };

  function showState(button, state) {
    button.innerHTML = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false">' + icons[state] + '</svg>';
    const label = document.createElement("span");
    label.className = "copy-label";
    label.textContent = labels[state];
    button.append(label);
    button.dataset.state = state;
  }

  function legacyCopy(text) {
    const focused = document.activeElement;
    const selection = window.getSelection();
    const ranges = [];
    if (selection) {
      for (let i = 0; i < selection.rangeCount; i++) {
        ranges.push(selection.getRangeAt(i).cloneRange());
      }
    }
    const input = document.createElement("textarea");
    input.className = "copy-fallback";
    input.value = text;
    input.readOnly = true;
    input.tabIndex = -1;
    input.setAttribute("aria-hidden", "true");
    document.body.append(input);
    try {
      input.focus({ preventScroll: true });
      input.select();
      return document.execCommand("copy");
    } finally {
      input.remove();
      if (focused) focused.focus({ preventScroll: true });
      if (selection) {
        selection.removeAllRanges();
        for (const range of ranges) selection.addRange(range);
      }
    }
  }

  async function copyText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      try {
        await navigator.clipboard.writeText(text);
        return true;
      } catch {
        // Try selection-based copying when clipboard permissions are denied.
      }
    }
    try {
      return legacyCopy(text);
    } catch {
      return false;
    }
  }

  for (const code of document.querySelectorAll(".prose pre > code")) {
    const pre = code.parentElement;
    if (pre.parentElement.classList.contains("code-block")) continue;

    const wrapper = document.createElement("div");
    wrapper.className = "code-block";
    const button = document.createElement("button");
    button.type = "button";
    button.className = "code-copy";
    showState(button, "copy");
    button.title = "Copy code to clipboard";
    button.setAttribute("aria-live", "polite");
    pre.replaceWith(wrapper);
    wrapper.append(button, pre);

    let busy = false;
    let reset;
    button.addEventListener("click", async () => {
      if (busy) return;
      busy = true;
      clearTimeout(reset);
      const copied = await copyText(code.textContent);
      showState(button, copied ? "copied" : "failed");
      button.title = copied ? "Copied!" : "Code selected. Use your browser's copy command.";
      if (!copied) {
        const selection = window.getSelection();
        const range = document.createRange();
        range.selectNodeContents(code);
        if (selection) {
          selection.removeAllRanges();
          selection.addRange(range);
        }
      }
      busy = false;
      reset = setTimeout(() => {
        showState(button, "copy");
        button.title = "Copy code to clipboard";
      }, 2000);
    });
  }
})();
