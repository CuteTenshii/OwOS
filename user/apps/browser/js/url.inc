/* URL and URLSearchParams, shared by the page (url.c) and dedicated workers (webworker.c); same conventions as
 * workerscope_prelude.inc (no single quotes, backslashes or // comments; every statement ends with a semicolon).
 * Called as f(globalThis, N) with the URL natives: defines the two classes on globalThis and returns them. */
JS(
(function (g, N) {
  var def = Object.defineProperty;
  function hide(o, k, v) { def(o, k, { value: v, writable: true, configurable: true, enumerable: false }); }
  function method(proto, name, fn) { hide(proto, name, fn); }
  function typeError(msg) { return new TypeError(msg); }

  /* ---- URLSearchParams ---- */
  function appendAll(list, pairs) {
    for (var i = 0; i < pairs.length; i++) { list.push(pairs[i]); }
  }
  function URLSearchParams(init) {
    var list = [], k;
    hide(this, "@list", list);
    hide(this, "@url", null);
    if (init === undefined || init === null) { return; }
    if (typeof init === "string") { appendAll(list, N.queryParse(init)); }
    else if (init instanceof URLSearchParams) { init["@list"].forEach(function (p) { list.push([p[0], p[1]]); }); }
    else if (typeof init === "object" && typeof init[Symbol.iterator] === "function") {
      for (var pair of init) {
        if (pair.length !== 2) { throw typeError("Failed to construct URLSearchParams: each pair must have two elements"); }
        list.push([String(pair[0]), String(pair[1])]);
      }
    } else if (typeof init === "object") {
      for (k in init) { list.push([k, String(init[k])]); }
    } else { appendAll(list, N.queryParse(String(init))); }
  }
  function touch(sp) { if (sp["@url"]) { sp["@url"]["@sync"](sp.toString()); } }
  method(URLSearchParams.prototype, "append", function (k, v) { this["@list"].push([String(k), String(v)]); touch(this); });
  method(URLSearchParams.prototype, "delete", function (k) {
    k = String(k);
    this["@list"].splice(0, this["@list"].length, ...this["@list"].filter(function (p) { return p[0] !== k; }));
    touch(this);
  });
  method(URLSearchParams.prototype, "get", function (k) {
    k = String(k);
    for (var i = 0; i < this["@list"].length; i++) { if (this["@list"][i][0] === k) { return this["@list"][i][1]; } }
    return null;
  });
  method(URLSearchParams.prototype, "getAll", function (k) {
    k = String(k);
    return this["@list"].filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; });
  });
  method(URLSearchParams.prototype, "has", function (k) {
    k = String(k);
    return this["@list"].some(function (p) { return p[0] === k; });
  });
  method(URLSearchParams.prototype, "set", function (k, v) {
    var l = this["@list"], done = false, out = [], i;
    k = String(k);
    v = String(v);
    for (i = 0; i < l.length; i++) {
      if (l[i][0] === k) { if (!done) { out.push([k, v]); done = true; } } else { out.push(l[i]); }
    }
    if (!done) { out.push([k, v]); }
    l.splice(0, l.length, ...out);
    touch(this);
  });
  method(URLSearchParams.prototype, "sort", function () {
    var l = this["@list"], sorted = l.map(function (p, i) { return [p, i]; });
    sorted.sort(function (a, b) { return a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]; });
    l.splice(0, l.length, ...sorted.map(function (e) { return e[0]; }));
    touch(this);
  });
  method(URLSearchParams.prototype, "forEach", function (fn, thisArg) {
    this["@list"].slice().forEach(function (p) { fn.call(thisArg, p[1], p[0], this); }, this);
  });
  method(URLSearchParams.prototype, "keys", function () { return this["@list"].map(function (p) { return p[0]; })[Symbol.iterator](); });
  method(URLSearchParams.prototype, "values", function () { return this["@list"].map(function (p) { return p[1]; })[Symbol.iterator](); });
  method(URLSearchParams.prototype, "entries", function () { return this["@list"].map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); });
  hide(URLSearchParams.prototype, Symbol.iterator, URLSearchParams.prototype.entries);
  method(URLSearchParams.prototype, "toString", function () {
    return N.queryString(this["@list"]);
  });
  def(URLSearchParams.prototype, "size", { get: function () { return this["@list"].length; }, configurable: true });
  hide(g, "URLSearchParams", URLSearchParams);

  /* ---- URL ---- */
  var SPECIAL = { "http:": "80", "https:": "443", "ftp:": "21", "ws:": "80", "wss:": "443", "file:": "" };
  function serialize(r) {
    var s = r.protocol;
    if (!r.opaque) {
      s += "//";
      if (r.username || r.password) { s += r.username + (r.password ? ":" + r.password : "") + "@"; }
      s += r.hostname + (r.port ? ":" + r.port : "");
    }
    return s + r.pathname + (r.search || (r.hasSearch ? "?" : "")) + (r.hash || (r.hasHash ? "#" : ""));
  }
  function URL(url, base) {
    if (arguments.length === 0) { throw typeError("Failed to construct URL: 1 argument required"); }
    var r = N.urlParse(String(url), base === undefined ? undefined : String(base instanceof URL ? base.href : base));
    if (!r) { throw typeError("Failed to construct URL: Invalid URL"); }
    hide(this, "@r", r);
    hide(this, "@sp", null);
  }
  URL.canParse = function (url, base) { try { new URL(url, base); return true; } catch (e) { return false; } };
  function urlProp(name, get, set) {
    def(URL.prototype, name, { get: get, set: set, configurable: true, enumerable: true });
  }
  function simple(name) {
    urlProp(name, function () { return this["@r"][name]; }, function (v) { this["@r"][name] = String(v); });
  }
  ["username", "password"].forEach(simple);
  urlProp("hostname", function () { return this["@r"].hostname; }, function (v) { this["@r"].hostname = String(v).toLowerCase(); });
  urlProp("protocol", function () { return this["@r"].protocol; }, function (v) {
    v = String(v);
    this["@r"].protocol = (v.charAt(v.length - 1) === ":" ? v : v + ":").toLowerCase();
  });
  urlProp("port", function () { return this["@r"].port; }, function (v) {
    v = String(v);
    this["@r"].port = SPECIAL[this["@r"].protocol] === v ? "" : v;
  });
  urlProp("host", function () { return this["@r"].hostname + (this["@r"].port ? ":" + this["@r"].port : ""); }, function (v) {
    var c = String(v).lastIndexOf(":");
    this["@r"].hostname = (c < 0 ? String(v) : String(v).slice(0, c)).toLowerCase();
    this["@r"].port = c < 0 ? "" : String(v).slice(c + 1);
  });
  urlProp("pathname", function () { return this["@r"].pathname; }, function (v) {
    v = String(v);
    this["@r"].pathname = N.urlEscape(v.charAt(0) === "/" ? v : "/" + v, 0);
  });
  urlProp("search", function () { return this["@r"].search; }, function (v) {
    v = String(v);
    this["@r"].hasSearch = v !== "";
    this["@r"].search = v === "" || v === "?" ? "" : N.urlEscape(v.charAt(0) === "?" ? v : "?" + v, 1);
    if (this["@sp"]) { this["@sp"]["@list"].splice(0, this["@sp"]["@list"].length, ...N.queryParse(this["@r"].search)); }
  });
  urlProp("hash", function () { return this["@r"].hash; }, function (v) {
    v = String(v);
    this["@r"].hasHash = v !== "";
    this["@r"].hash = v === "" || v === "#" ? "" : N.urlEscape(v.charAt(0) === "#" ? v : "#" + v, 2);
  });
  urlProp("href", function () { return serialize(this["@r"]); }, function (v) {
    var r = N.urlParse(String(v));
    if (!r) { throw typeError("Invalid URL"); }
    this["@r"] = r;
  });
  urlProp("origin", function () {
    var r = this["@r"];
    return SPECIAL[r.protocol] !== undefined && r.protocol !== "file:" ? r.protocol + "//" + r.hostname + (r.port ? ":" + r.port : "") : "null";
  }, undefined);
  urlProp("searchParams", function () {
    if (!this["@sp"]) {
      var sp = new URLSearchParams(this["@r"].search);
      sp["@url"] = this;
      this["@sp"] = sp;
    }
    return this["@sp"];
  }, undefined);
  hide(URL.prototype, "@sync", function (q) { this["@r"].search = q ? "?" + q : ""; this["@r"].hasSearch = !!q; });
  method(URL.prototype, "toString", function () { return this.href; });
  method(URL.prototype, "toJSON", function () { return this.href; });
  hide(g, "URL", URL);
  return { URL: URL, URLSearchParams: URLSearchParams };
})
)
