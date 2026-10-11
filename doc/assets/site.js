/* Offline knowledge base: source Markdown is bundled by build.py. */
(() => {
  "use strict";
  const $ = (selector) => document.querySelector(selector);
  const main = $("#main");
  const data = window.EPD_DOCS;
  if (!data || !window.marked || !window.DOMPurify || !window.DocCore) {
    main.textContent =
      "知识库文件不完整，请重新获取 doc 目录，或打开仓库 docs/README.md。";
    return;
  }
  const { route, parseRoute, resolveLink, slugger, indexDocuments, search } =
    window.DocCore;
  const docs = new Map(data.documents.map((doc) => [doc.id, doc]));
  const ids = new Set(docs.keys());
  const searchIndex = indexDocuments(data.documents);
  const dialog = $("#search-dialog");
  const sidebar = $("#sidebar");
  const toc = $("#toc");
  let currentId = null;
  let observer;
  let noticeTimer;
  let searchTimer;

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function link(text, href, className) {
    const node = element("a", className, text);
    node.href = href;
    return node;
  }
  function notify(text) {
    clearTimeout(noticeTimer);
    $("#notice").textContent = text;
    $("#notice").classList.add("show");
    noticeTimer = setTimeout(() => $("#notice").classList.remove("show"), 2200);
  }
  function setMenu(open) {
    sidebar.classList.toggle("open", open);
    document.body.classList.toggle("nav-open", open);
    $("#nav-backdrop").hidden = !open;
    $("#menu-toggle").setAttribute("aria-expanded", String(open));
    $("#menu-toggle").setAttribute(
      "aria-label",
      open ? "关闭文档目录" : "打开文档目录",
    );
    main.inert = open;
    toc.inert = open;
  }

  function buildNavigation() {
    data.groups.forEach((group, n) => {
      const section = element("details", "nav-group");
      section.open = n < 3;
      section.append(element("summary", "", group.title));
      group.ids.forEach((id) => {
        const node = link(docs.get(id).title, route(id), "nav-link");
        node.dataset.id = id;
        section.append(node);
      });
      $("#navigation").append(section);
    });
  }
  function activeNavigation(id) {
    document.querySelectorAll(".nav-link").forEach((node) => {
      const active = node.dataset.id === id;
      node.classList.toggle("active", active);
      if (active) {
        node.setAttribute("aria-current", "page");
        node.closest("details").open = true;
      } else node.removeAttribute("aria-current");
    });
    $(".nav-home").classList.toggle("active", !id);
    if (!id) $(".nav-home").setAttribute("aria-current", "page");
    else $(".nav-home").removeAttribute("aria-current");
  }
  function breadcrumb(group, title) {
    const node = element("div", "breadcrumb");
    node.append(link("知识库", "#/"));
    if (group)
      node.append(element("span", "", "/"), element("span", "", group));
    if (title)
      node.append(element("span", "", "/"), element("span", "", title));
    return node;
  }
  function footer() {
    const node = element("footer", "footer");
    node.append(
      element("span", "", "SiFli EPD · 平台开发文档"),
      link("知识库维护", route("doc/README.md")),
    );
    return node;
  }
  function heading(title, note) {
    const row = element("div", "section-head");
    row.append(element("h2", "", title), element("span", "", note));
    return row;
  }
  function home() {
    main.append(breadcrumb("文档首页"));
    const hero = element("section", "hero");
    hero.append(
      element("div", "eyebrow", "SIFLI EPD / DOCUMENTATION"),
      element("h1", "home-title", "从理解平台，到构建应用。"),
      element(
        "p",
        "home-intro",
        "墨水屏平台的架构、接口与开发实践。在这里了解系统如何组织，构建你的第一个动态应用，或查阅下一阶段的技术设计。",
      ),
    );
    const actions = element("div", "hero-actions");
    actions.append(
      link("阅读平台架构  →", route("docs/ARCHITECTURE.md"), "button"),
      link("开始构建", route("docs/BUILDING.md"), "button secondary"),
    );
    hero.append(actions);
    main.append(
      hero,
      heading(
        "按主题查阅",
        `${data.groups.length} 个主题 · ${docs.size} 篇文档`,
      ),
    );
    const grid = element("div", "card-grid");
    [0, 1, 2, 4, 5, 6].forEach((index, i) => {
      const group = data.groups[index];
      const card = link("", route(group.ids[0]), "topic-card");
      const top = element("div", "card-top");
      top.append(
        element("span", "card-number", String(i + 1).padStart(2, "0")),
        element("span", "card-arrow", "↗"),
      );
      card.append(
        top,
        element("h3", "", group.title),
        element("p", "", group.description),
        element("small", "", `${group.ids.length} 篇文档`),
      );
      grid.append(card);
    });
    main.append(grid, heading("应用设计", "从具体应用了解平台实践"));
    const apps = element("div", "app-links");
    [
      ["weather", "天气"],
      ["books", "书架"],
      ["gallery", "图库"],
      ["words", "单词"],
      ["weread", "微信读书"],
      ["board", "信息看板"],
    ].forEach(([id, name]) => {
      const item = link(name, route(`modules/${id}/DESIGN.md`), "app-link");
      item.append(element("span", "", "↗"));
      apps.append(item);
    });
    main.append(apps);
    const note = element("div", "home-note");
    note.append(
      "实施状态与测试结论见",
      link("平台验收记录", route("docs/PLATFORM_VALIDATION.md")),
      "；低功耗、OTA 等后续能力集中在",
      link("技术储备", route("docs/TECHNICAL_RESERVES.md")),
      "中查阅。",
    );
    main.append(note, footer());
    toc.append(element("h2", "", "快速开始"));
    [
      ["了解系统边界", "docs/ARCHITECTURE.md"],
      ["编译与工厂预装", "docs/BUILDING.md"],
      ["开发动态应用", "docs/APPLICATION_DEVELOPMENT.md"],
      ["定位应用故障", "docs/DYNAMIC_APP_DEBUGGING.md"],
    ].forEach(([text, id], i) =>
      toc.append(link(`${i + 1}. ${text}`, route(id))),
    );
    const extra = element("div", "toc-extra");
    extra.append(
      element("h2", "", "文档与源码同行"),
      element(
        "p",
        "",
        "正文来自仓库 Markdown。网页提供统一的导航、搜索和阅读入口。",
      ),
      link("浏览完整文档索引 →", route("docs/README.md")),
    );
    toc.append(extra);
  }

  async function copyCode(code, button) {
    try {
      if (navigator.clipboard && window.isSecureContext)
        await navigator.clipboard.writeText(code);
      else {
        const input = element("textarea");
        input.value = code;
        input.style.cssText = "position:fixed;left:-9999px;top:0";
        document.body.append(input);
        input.select();
        const ok = document.execCommand("copy");
        input.remove();
        button.focus();
        if (!ok) throw new Error("clipboard unavailable");
      }
      notify("代码已复制");
    } catch (_) {
      notify("浏览器未允许复制，请选中代码后复制。");
    }
  }
  function article(doc) {
    main.append(breadcrumb(doc.group));
    const meta = element("div", "article-meta");
    meta.append(
      element("span", "", doc.id),
      link("Markdown 源文件 ↗", "../" + doc.id),
    );
    main.append(meta);
    const body = element("article", "article");
    body.innerHTML = DOMPurify.sanitize(
      marked.parse(doc.markdown, { gfm: true }),
      {
        USE_PROFILES: { html: true },
        FORBID_TAGS: ["style", "form", "button", "textarea", "iframe"],
        FORBID_ATTR: ["style", "id", "name"],
      },
    );
    body.querySelectorAll("a[href]").forEach((node) => {
      const href = resolveLink(doc.id, node.getAttribute("href"), ids);
      if (href === null) node.removeAttribute("href");
      else {
        node.setAttribute("href", href);
        if (/^https?:/i.test(href)) {
          node.target = "_blank";
          node.rel = "noopener noreferrer";
        }
      }
    });
    body.querySelectorAll("img").forEach((node) => {
      const src = node.getAttribute("src") || "";
      const target = resolveLink(doc.id, src, new Set());
      // External image fetches are explicit links so opening offline remains self-contained.
      if (!target || !target.startsWith("../")) {
        node.replaceWith(
          target
            ? link(node.alt || "外部图片", target)
            : element("span", "", node.alt),
        );
      } else node.src = target;
    });
    body.querySelectorAll("table").forEach((table) => {
      const wrapper = element("div", "table-scroll");
      wrapper.tabIndex = 0;
      wrapper.setAttribute("role", "region");
      wrapper.setAttribute("aria-label", "表格，可横向滚动");
      table.replaceWith(wrapper);
      wrapper.append(table);
    });
    body.querySelectorAll("pre").forEach((pre) => {
      const code = pre.textContent;
      const button = element("button", "copy-button", "复制");
      button.setAttribute("aria-label", "复制代码");
      button.addEventListener("click", () => copyCode(code, button));
      pre.prepend(button);
    });
    const slug = slugger();
    const headings = [...body.querySelectorAll("h1,h2,h3,h4,h5,h6")];
    toc.append(element("h2", "", "本页目录"));
    headings.forEach((node) => {
      const title = node.textContent;
      node.id = slug(title);
      const anchor = link("#", route(doc.id, node.id), "heading-anchor");
      anchor.setAttribute("aria-label", `定位章节：${title}`);
      node.append(anchor);
      if (node.tagName === "H2" || node.tagName === "H3") {
        const item = link(
          title,
          route(doc.id, node.id),
          node.tagName === "H3" ? "sub" : "",
        );
        item.dataset.heading = node.id;
        toc.append(item);
      }
    });
    if (!toc.querySelector("a"))
      toc.append(element("p", "", "本文没有子章节。"));
    const extra = element("div", "toc-extra");
    extra.append(
      link("回到顶部 ↑", route(doc.id)),
      link("Markdown 源文件 ↗", "../" + doc.id),
    );
    toc.append(extra);
    main.append(body);
    const position = data.documents.findIndex((item) => item.id === doc.id);
    const neighbors = element("div", "page-links");
    [
      [position - 1, "上一篇"],
      [position + 1, "下一篇"],
    ].forEach(([index, label]) => {
      const next = data.documents[index];
      if (!next) {
        neighbors.append(element("span"));
        return;
      }
      const item = link("", route(next.id));
      item.append(
        element("small", "", label),
        document.createTextNode(next.title),
      );
      neighbors.append(item);
    });
    main.append(neighbors, footer());
    observer = new IntersectionObserver(
      (entries) => {
        const visible = entries.filter((entry) => entry.isIntersecting);
        if (!visible.length) return;
        const id = visible[0].target.id;
        toc
          .querySelectorAll("[data-heading]")
          .forEach((node) =>
            node.classList.toggle("current", node.dataset.heading === id),
          );
      },
      { rootMargin: "-80px 0px -65% 0px" },
    );
    headings.forEach((node) => observer.observe(node));
  }

  function render() {
    const selected = parseRoute(location.hash);
    // The skip link is not a documentation route.
    if (location.hash === "#main") {
      main.focus();
      return;
    }
    if (selected.id !== currentId) {
      observer?.disconnect();
      currentId = selected.id;
      main.replaceChildren();
      toc.replaceChildren();
      activeNavigation(currentId);
      if (!currentId) home();
      else if (docs.has(currentId)) article(docs.get(currentId));
      else {
        const missing = element("div", "missing");
        missing.append(
          element("h1", "", "未找到这篇文档"),
          element("p", "", "请从左侧目录或搜索中重新选择文档。"),
          link("返回知识库首页 →", "#/"),
        );
        main.append(missing);
      }
      document.title =
        currentId && docs.has(currentId)
          ? `${docs.get(currentId).title} · ${data.title}`
          : data.title;
    }
    setMenu(false);
    if (dialog.open) dialog.close();
    requestAnimationFrame(() => {
      const target =
        selected.section && document.getElementById(selected.section);
      if (target && main.contains(target)) target.scrollIntoView();
      else window.scrollTo(0, 0);
    });
  }
  function highlight(node, text, query) {
    const terms = query.toLowerCase().trim().split(/\s+/).filter(Boolean);
    const lower = text.toLowerCase();
    let offset = 0;
    while (offset < text.length) {
      let start = -1,
        term = "";
      terms.forEach((candidate) => {
        const at = lower.indexOf(candidate, offset);
        if (at >= 0 && (start < 0 || at < start)) {
          start = at;
          term = candidate;
        }
      });
      if (start < 0) {
        node.append(document.createTextNode(text.slice(offset)));
        break;
      }
      node.append(
        document.createTextNode(text.slice(offset, start)),
        element("mark", "", text.slice(start, start + term.length)),
      );
      offset = start + term.length;
    }
  }
  function updateSearch() {
    const query = $("#search-input").value;
    const results = search(searchIndex, query);
    $("#search-results").replaceChildren();
    $("#search-status").textContent = !query.trim()
      ? `在 ${docs.size} 篇文档中查找`
      : results.length
        ? `找到 ${results.length} 篇相关文档`
        : "没有找到匹配文档，请尝试其他关键词。";
    results.forEach((result) => {
      const item = link("", route(result.id), "search-result");
      item.append(element("small", "", result.group));
      const title = element("strong");
      const snippet = element("p");
      highlight(title, result.title, query);
      highlight(snippet, result.snippet, query);
      item.append(title, snippet);
      item.addEventListener("click", () => {
        dialog.close();
        if (location.hash === item.hash) render();
      });
      $("#search-results").append(item);
    });
  }
  function openSearch() {
    setMenu(false);
    dialog.showModal();
    updateSearch();
    $("#search-input").focus();
    $("#search-input").select();
  }

  $("#search-open").addEventListener("click", openSearch);
  $("#search-close").addEventListener("click", () => dialog.close());
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) dialog.close();
  });
  $("#search-input").addEventListener("input", () => {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(updateSearch, 100);
  });
  $("#search-input").addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      $(".search-result")?.focus();
    }
    if (event.key === "Enter") {
      event.preventDefault();
      $(".search-result")?.click();
    }
  });
  $("#menu-toggle").addEventListener("click", () =>
    setMenu(!sidebar.classList.contains("open")),
  );
  $("#nav-backdrop").addEventListener("click", () => {
    setMenu(false);
    $("#menu-toggle").focus();
  });
  sidebar.addEventListener("click", (event) => {
    if (event.target.closest("a")) setMenu(false);
  });
  document.addEventListener("keydown", (event) => {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      openSearch();
    }
    if (event.key === "Escape" && sidebar.classList.contains("open")) {
      setMenu(false);
      $("#menu-toggle").focus();
    }
  });
  window.addEventListener("hashchange", render);
  window
    .matchMedia("(max-width:760px)")
    .addEventListener("change", () => setMenu(false));
  buildNavigation();
  render();
})();
