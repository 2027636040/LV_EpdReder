const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const core = require("../assets/core.js");
const marked = require("../vendor/marked.umd.js");

test("route round trips Chinese paths, anchors and reserved characters", () => {
  const id = "docs/中文 #?.md",
    section = "章节-1";
  assert.deepEqual(core.parseRoute(core.route(id, section)), { id, section });
  assert.deepEqual(core.parseRoute("#/"), { id: "", section: "" });
  assert.doesNotThrow(() => core.parseRoute("#/%bad%"));
});

test("internal Markdown links resolve against the source, not the viewer", () => {
  const ids = new Set(["docs/BUILDING.md", "modules/books/DESIGN.md"]);
  assert.equal(
    core.resolveLink("docs/README.md", "BUILDING.md#快速构建", ids),
    core.route("docs/BUILDING.md", "快速构建"),
  );
  assert.equal(
    core.resolveLink("docs/README.md", "../modules/books/DESIGN.md", ids),
    core.route("modules/books/DESIGN.md"),
  );
  assert.equal(
    core.resolveLink("docs/BUILDING.md", "#编译", ids),
    core.route("docs/BUILDING.md", "编译"),
  );
  assert.equal(
    core.resolveLink("modules/books/DESIGN.md", "../../src/main.c", ids),
    "../src/main.c",
  );
  assert.equal(core.resolveLink("README.md", "doc/index.html", ids), "#/");
  assert.equal(core.resolveLink("README.md", "../outside.md", ids), null);
  assert.equal(core.resolveLink("README.md", "javascript:alert(1)", ids), null);
  assert.equal(core.resolveLink("README.md", "//example.org/a", ids), null);
  assert.equal(
    core.resolveLink("README.md", "https://example.org", ids),
    "https://example.org",
  );
  assert.equal(core.resolveLink("README.md", "file:///C:/secret", ids), null);
});

test("heading IDs are stable and unique, including numbered duplicates", () => {
  const slug = core.slugger();
  assert.equal(slug("应用生命周期"), "应用生命周期");
  assert.equal(slug("API: app_fwk / LVGL"), "api-app_fwk--lvgl");
  assert.equal(slug("A"), "a");
  assert.equal(slug("A"), "a-1");
  assert.equal(slug("A-1"), "a-1-1");
});

test("full text search supports Chinese, case folding and multiple terms", () => {
  const index = core.indexDocuments([
    {
      id: "a",
      title: "生命周期",
      group: "开发",
      markdown: "# 应用\nPSRAM 与后台线程",
    },
    { id: "b", title: "存储", group: "开发", markdown: "生命周期中的 PSRAM" },
  ]);
  assert.deepEqual(
    core.search(index, "生命周期").map((item) => item.id),
    ["a", "b"],
  );
  assert.equal(core.search(index, "psram 后台")[0].id, "a");
  assert.equal(core.search(index, "不存在").length, 0);
  assert.equal(core.search(index, "  ").length, 0);
});

const context = { window: {} };
vm.runInNewContext(
  fs.readFileSync(path.join(__dirname, "../assets/content.js"), "utf8"),
  context,
);
const bundle = context.window.EPD_DOCS;
test("every bundled document renders using the pinned Markdown parser", () => {
  for (const doc of bundle.documents)
    assert.equal(typeof marked.parse(doc.markdown), "string", doc.id);
});

test("all relative document links resolve to a route or a real repository file", () => {
  const ids = new Set(bundle.documents.map((doc) => doc.id));
  const root = path.resolve(__dirname, "../..");
  const failures = [];
  for (const doc of bundle.documents) {
    marked.walkTokens(marked.lexer(doc.markdown), (token) => {
      if (token.type !== "link" && token.type !== "image") return;
      const href = core.resolveLink(doc.id, token.href, ids);
      if (href?.startsWith("../")) {
        const target = decodeURIComponent(href.split(/[?#]/, 1)[0].slice(3));
        if (!fs.existsSync(path.join(root, target)))
          failures.push(`${doc.id}: ${token.href}`);
      }
    });
  }
  assert.deepEqual(failures, []);
});

test("offline viewer only includes local runtime scripts and styles", () => {
  const html = fs.readFileSync(path.join(__dirname, "../index.html"), "utf8");
  for (const match of html.matchAll(/(?:src|href)="([^"#]+)"/g)) {
    if (match[1].startsWith("data:") || match[1].startsWith("../")) continue;
    assert.ok(!/^https?:/.test(match[1]));
    assert.ok(fs.existsSync(path.join(__dirname, "..", match[1])), match[1]);
  }
});
