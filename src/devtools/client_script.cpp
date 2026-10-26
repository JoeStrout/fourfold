// The script dev mode adds to every HTML page (see dev_tools.h). It builds
// the overlay with textContent only, so nothing in an error message or a
// line of code can become markup.

#include "devtools/dev_tools.h"

namespace fourfold {

const char* const kDevClientScript = R"js((function () {
  "use strict";
  var me = document.currentScript;
  var generation = Number((me && me.dataset.generation) || 0);
  var overlay = null;

  function el(tag, style, text) {
    var e = document.createElement(tag);
    for (var k in style) e.style[k] = style[k];
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function hideErrors() {
    if (overlay) overlay.remove();
    overlay = null;
  }

  function showErrors(list) {
    hideErrors();
    if (!list || !list.length) return;
    overlay = el("div", {
      position: "fixed", inset: "0", zIndex: "2147483647", overflow: "auto",
      background: "rgba(20, 20, 24, 0.94)", color: "#eee",
      font: "15px/1.45 -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif"
    });
    var main = el("div", { maxWidth: "60rem", margin: "0 auto", padding: "2rem 1.25rem" });
    overlay.appendChild(main);
    var title = list.length === 1 ? "The app didn't compile" : "The app didn't compile (" + list.length + " errors)";
    main.appendChild(el("h1", { fontSize: "1.15rem", color: "#ff8a80", margin: "0 0 1rem" }, title));
    list.forEach(function (d) {
      var where = d.file + (d.line > 0 ? ", line " + d.line : "");
      main.appendChild(el("div", { color: "#aaa", fontFamily: "ui-monospace, Menlo, monospace" }, where));
      main.appendChild(el("div", { fontSize: "1.05rem", margin: "0.25rem 0 0.75rem", whiteSpace: "pre-wrap" }, d.message));
      if (d.code && d.code.length) {
        var pre = el("pre", {
          background: "#111", borderRadius: "6px", padding: "0.75rem 0", overflowX: "auto",
          font: "13px/1.5 ui-monospace, Menlo, monospace", margin: "0 0 1.5rem"
        });
        d.code.forEach(function (text, i) {
          var n = d.first + i;
          var row = el("div", { padding: "0 1rem", whiteSpace: "pre", background: n === d.line ? "#5c1f1f" : "" });
          row.appendChild(el("span", { display: "inline-block", width: "3.5em", color: "#777", userSelect: "none" }, String(n)));
          row.appendChild(document.createTextNode(text));
          pre.appendChild(row);
        });
        main.appendChild(pre);
      }
    });
    main.appendChild(el("p", { color: "#888", fontSize: "0.85rem" },
      "The site keeps serving the last version that compiled. Fix the error and save; this page reloads."));
    (document.body || document.documentElement).appendChild(overlay);
  }

  // Template warnings: a small panel in the corner, until dismissed (or
  // more arrive).
  var warningPanel = null, dismissed = 0;

  function showWarnings(list) {
    if (warningPanel) warningPanel.remove();
    warningPanel = null;
    if (!list || !list.length || list.length <= dismissed) return;
    warningPanel = el("div", {
      position: "fixed", right: "1rem", bottom: "1rem", zIndex: "2147483646", maxWidth: "32rem",
      maxHeight: "40vh", overflow: "auto", background: "#2b2611", color: "#f5e6a8",
      border: "1px solid #6b5a1a", borderRadius: "6px", padding: "0.6rem 0.9rem",
      font: "13px/1.4 -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif",
      boxShadow: "0 4px 16px rgba(0,0,0,0.3)"
    });
    var close = el("button", {
      float: "right", background: "none", border: "0", color: "inherit", cursor: "pointer",
      font: "inherit", fontSize: "1.1rem", lineHeight: "1", padding: "0 0 0 0.5rem"
    }, "×");
    close.title = "Dismiss";
    close.addEventListener("click", function () {
      dismissed = list.length;
      warningPanel.remove();
      warningPanel = null;
    });
    warningPanel.appendChild(close);
    warningPanel.appendChild(el("div", { fontWeight: "600", marginBottom: "0.3rem" },
      list.length === 1 ? "1 template warning" : list.length + " template warnings"));
    list.forEach(function (w) {
      var row = el("div", { margin: "0.2rem 0" });
      row.appendChild(el("span", { fontFamily: "ui-monospace, Menlo, monospace", color: "#c9b56a" },
        w.file + (w.line > 0 ? ":" + w.line : "") + " "));
      row.appendChild(document.createTextNode(w.message));
      warningPanel.appendChild(row);
    });
    (document.body || document.documentElement).appendChild(warningPanel);
  }

  function swapStylesheets(urls, next) {
    var links = document.querySelectorAll("link[rel~='stylesheet']");
    for (var i = 0; i < links.length; i++) {
      var url = new URL(links[i].href, location.href);
      if (url.origin === location.origin && urls.indexOf(url.pathname) >= 0) {
        url.searchParams.set("__ff", String(next));
        links[i].href = url.href;
      }
    }
  }

  function data(event) {
    try { return JSON.parse(event.data); } catch (e) { return {}; }
  }

  var source = new EventSource("/__dev/reload");
  source.addEventListener("hello", function (e) {
    var d = data(e);
    if (d.generation !== generation) { location.reload(); return; }   // changed while we weren't listening
    showErrors(d.diagnostics);
    showWarnings(d.warnings);
  });
  source.addEventListener("warnings", function (e) {
    var d = data(e);
    if (d.generation === generation) showWarnings(d.warnings);
  });
  source.addEventListener("reload", function () { location.reload(); });
  source.addEventListener("css", function (e) {
    var d = data(e);
    swapStylesheets(d.urls || [], d.generation);
    generation = d.generation;
    hideErrors();
  });
  source.addEventListener("errors", function (e) { showErrors(data(e).diagnostics); });
})();
)js";

}  // namespace fourfold
