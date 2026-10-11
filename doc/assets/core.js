(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.DocCore = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  function decode(value) {
    try {
      return decodeURIComponent(value);
    } catch (_) {
      return value;
    }
  }

  function route(id, section = "") {
    return (
      "#/" +
      id.split("/").map(encodeURIComponent).join("/") +
      (section ? "?section=" + encodeURIComponent(section) : "")
    );
  }

  function parseRoute(hash) {
    const value = hash.replace(/^#\/?/, "");
    const split = value.indexOf("?");
    return {
      id: decode(split < 0 ? value : value.slice(0, split)),
      section:
        new URLSearchParams(split < 0 ? "" : value.slice(split + 1)).get(
          "section",
        ) || "",
    };
  }

  function resolveLink(source, href, ids) {
    if (/^[a-z][a-z\d+.-]*:/i.test(href) || href.startsWith("//")) {
      return /^(https?:|mailto:)/i.test(href) ? href : null;
    }
    const hashAt = href.indexOf("#");
    const rawPath = hashAt < 0 ? href : href.slice(0, hashAt);
    const section = hashAt < 0 ? "" : decode(href.slice(hashAt + 1));
    if (!rawPath) return route(source, section);
    const queryAt = rawPath.indexOf("?");
    const query = queryAt < 0 ? "" : rawPath.slice(queryAt);
    const path = decode(queryAt < 0 ? rawPath : rawPath.slice(0, queryAt));
    const parts = path.startsWith("/") ? [] : source.split("/").slice(0, -1);
    for (const part of path.split("/")) {
      if (part === "..") {
        if (!parts.length) return null;
        parts.pop();
      } else if (part && part !== ".") parts.push(part);
    }
    const target = parts.join("/");
    if (ids.has(target)) return route(target, section);
    if (target === "doc/index.html") return "#/";
    return (
      "../" +
      parts.map(encodeURIComponent).join("/") +
      query +
      (section ? "#" + encodeURIComponent(section) : "")
    );
  }

  function slugger() {
    const used = new Set();
    return (text) => {
      const base =
        text
          .trim()
          .toLowerCase()
          .replace(/[^\p{L}\p{N}\p{M}_\-\s]/gu, "")
          .replace(/\s/g, "-") || "section";
      let id = base,
        n = 0;
      while (used.has(id)) id = base + "-" + ++n;
      used.add(id);
      return id;
    };
  }

  function plain(markdown) {
    return markdown
      .replace(/```[^\n]*\n/g, "\n")
      .replace(/```/g, "")
      .replace(/!?\[([^\]]*)\]\([^)]*\)/g, "$1")
      .replace(/<[^>]*>/g, " ")
      .replace(/[#*`>|]/g, " ")
      .replace(/\s+/g, " ")
      .trim();
  }

  function indexDocuments(documents) {
    return documents.map((doc) => {
      const body = plain(doc.markdown);
      return {
        ...doc,
        body,
        lowerBody: body.toLowerCase(),
        lowerTitle: doc.title.toLowerCase(),
      };
    });
  }

  function search(index, query) {
    const terms = query.trim().toLowerCase().split(/\s+/).filter(Boolean);
    if (!terms.length) return [];
    return index
      .flatMap((doc) => {
        if (
          !terms.every(
            (term) =>
              doc.lowerTitle.includes(term) || doc.lowerBody.includes(term),
          )
        )
          return [];
        const score = terms.reduce(
          (sum, term) =>
            sum +
            (doc.lowerTitle.includes(term) ? 20 : 0) +
            (doc.lowerTitle === term ? 40 : 0) +
            (doc.lowerBody.includes(term) ? 1 : 0),
          0,
        );
        const first = terms
          .map((term) => doc.lowerBody.indexOf(term))
          .filter((i) => i >= 0);
        const start = Math.max(0, (first.length ? Math.min(...first) : 0) - 36);
        return [
          {
            id: doc.id,
            title: doc.title,
            group: doc.group,
            score,
            snippet:
              (start ? "…" : "") +
              doc.body.slice(start, start + 160) +
              (start + 160 < doc.body.length ? "…" : ""),
          },
        ];
      })
      .sort((a, b) => b.score - a.score || a.id.localeCompare(b.id));
  }

  return {
    route,
    parseRoute,
    resolveLink,
    slugger,
    plain,
    indexDocuments,
    search,
  };
});
