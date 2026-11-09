/* URL, URLSearchParams, Blob, File, FormData and Headers for a dedicated worker; same conventions as
 * workerscope_prelude.inc (no single quotes, backslashes or // comments; every statement ends with a semicolon). */
JS(
(function (g, N, L) {
  var hide = L.hide, method = L.method, typeError = L.typeError, def = Object.defineProperty;
  var QUOTE = String.fromCharCode(39);

  /* ---- percent coding ---- */
  function hex2(n) { return (n < 16 ? "0" : "") + n.toString(16).toUpperCase(); }
  function escapeWith(s, isBad) {
    var out = "", i, c, enc;
    for (i = 0; i < s.length; i++) {
      c = s.charCodeAt(i);
      if (c > 126 || c < 33 || isBad(c)) {
        if (c >= 55296 && c <= 56319 && i + 1 < s.length) { enc = encodeURIComponent(s.charAt(i) + s.charAt(i + 1)); i++; }
        else if (c > 127) { enc = encodeURIComponent(s.charAt(i)); }
        else { enc = "%" + hex2(c); }
        out += enc;
      } else { out += s.charAt(i); }
    }
    return out;
  }
  function badPath(c) { return c === 34 || c === 60 || c === 62 || c === 96 || c === 123 || c === 125 || c === 63 || c === 35; }
  function badQuery(c) { return c === 34 || c === 60 || c === 62 || c === 35 || c === 39; }
  function badFragment(c) { return c === 34 || c === 60 || c === 62 || c === 96; }
  function formEncode(s) {
    var e = encodeURIComponent(String(s)).split("%20").join("+");
    e = e.split("!").join("%21").split("~").join("%7E").split("(").join("%28").split(")").join("%29").split(QUOTE).join("%27");
    return e;
  }
  function formDecode(s) {
    s = s.split("+").join(" ");
    try { return decodeURIComponent(s); } catch (e) { return s; }
  }

  /* ---- URLSearchParams ---- */
  function parseQuery(q) {
    var list = [], parts, i, p, eq;
    if (q.charAt(0) === "?") { q = q.slice(1); }
    if (!q) { return list; }
    parts = q.split("&");
    for (i = 0; i < parts.length; i++) {
      p = parts[i];
      if (!p) { continue; }
      eq = p.indexOf("=");
      list.push(eq < 0 ? [formDecode(p), ""] : [formDecode(p.slice(0, eq)), formDecode(p.slice(eq + 1))]);
    }
    return list;
  }
  function URLSearchParams(init) {
    var list = [], k;
    hide(this, "@list", list);
    hide(this, "@url", null);
    if (init === undefined || init === null) { return; }
    if (typeof init === "string") { list.push.apply(list, parseQuery(init)); }
    else if (init instanceof URLSearchParams) { init["@list"].forEach(function (p) { list.push([p[0], p[1]]); }); }
    else if (typeof init === "object" && typeof init[Symbol.iterator] === "function") {
      for (var pair of init) {
        if (pair.length !== 2) { throw typeError("Failed to construct URLSearchParams: each pair must have two elements"); }
        list.push([String(pair[0]), String(pair[1])]);
      }
    } else if (typeof init === "object") {
      for (k in init) { list.push([k, String(init[k])]); }
    } else { list.push.apply(list, parseQuery(String(init))); }
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
    return this["@list"].map(function (p) { return formEncode(p[0]) + "=" + formEncode(p[1]); }).join("&");
  });
  def(URLSearchParams.prototype, "size", { get: function () { return this["@list"].length; }, configurable: true });
  hide(g, "URLSearchParams", URLSearchParams);

  /* ---- URL ---- */
  var SPECIAL = { "http:": "80", "https:": "443", "ftp:": "21", "ws:": "80", "wss:": "443", "file:": "" };
  function parseAbsolute(s) {
    var r = { protocol: "", username: "", password: "", hostname: "", port: "", pathname: "", search: "", hash: "", opaque: false };
    var i = s.indexOf("#"), rest, auth, at, colon, end, slash;
    if (i >= 0) { r.hash = s.slice(i); s = s.slice(0, i); }
    i = s.indexOf("?");
    if (i >= 0) { r.search = s.slice(i); s = s.slice(0, i); }
    i = s.indexOf(":");
    if (i < 1) { return null; }
    r.protocol = s.slice(0, i + 1).toLowerCase();
    rest = s.slice(i + 1);
    if (rest.slice(0, 2) === "//") {
      rest = rest.slice(2);
      slash = rest.indexOf("/");
      auth = slash < 0 ? rest : rest.slice(0, slash);
      r.pathname = slash < 0 ? "" : rest.slice(slash);
      at = auth.lastIndexOf("@");
      if (at >= 0) {
        var ui = auth.slice(0, at);
        auth = auth.slice(at + 1);
        colon = ui.indexOf(":");
        r.username = colon < 0 ? ui : ui.slice(0, colon);
        r.password = colon < 0 ? "" : ui.slice(colon + 1);
      }
      if (auth.charAt(0) === "[") {
        end = auth.indexOf("]");
        r.hostname = auth.slice(0, end + 1).toLowerCase();
        if (auth.charAt(end + 1) === ":") { r.port = auth.slice(end + 2); }
      } else {
        colon = auth.lastIndexOf(":");
        if (colon >= 0) { r.port = auth.slice(colon + 1); auth = auth.slice(0, colon); }
        r.hostname = auth.toLowerCase();
      }
      if (r.port !== "" && SPECIAL[r.protocol] === r.port) { r.port = ""; }
      if (r.pathname === "" && SPECIAL[r.protocol] !== undefined) { r.pathname = "/"; }
    } else {
      r.pathname = rest;
      r.opaque = true;
    }
    if (!r.opaque) { r.pathname = escapeWith(r.pathname, badPath).split("%25").join("%25"); }
    r.search = r.search === "?" ? "" : escapeWith(r.search, badQuery);
    r.hash = r.hash === "#" ? "" : escapeWith(r.hash, badFragment);
    return r;
  }
  function serialize(r) {
    var s = r.protocol;
    if (!r.opaque) {
      s += "//";
      if (r.username || r.password) { s += r.username + (r.password ? ":" + r.password : "") + "@"; }
      s += r.hostname + (r.port ? ":" + r.port : "");
    }
    return s + r.pathname + r.search + r.hash;
  }
  function removeDots(path) {
    var segs = path.split("/"), out = [], i, seg, last;
    for (i = 0; i < segs.length; i++) {
      seg = segs[i];
      last = i === segs.length - 1;
      if (seg === "..") { if (out.length > 1) { out.pop(); } if (last) { out.push(""); } }
      else if (seg === ".") { if (last) { out.push(""); } }
      else { out.push(seg); }
    }
    return out.join("/");
  }
  function hasScheme(s) {
    var i, c;
    if (!/^[A-Za-z]/.test(s)) { return false; }
    for (i = 1; i < s.length; i++) {
      c = s.charAt(i);
      if (c === ":") { return true; }
      if (!/[A-Za-z0-9+.-]/.test(c)) { return false; }
    }
    return false;
  }
  /* RFC 3986 reference resolution; returns an absolute URL string or null */
  function resolveRef(ref, base) {
    var cut, tail = "", path, q, h, root;
    ref = ref.trim();
    if (hasScheme(ref)) { return ref; }
    if (!base) { return null; }
    if (ref.slice(0, 2) === "//") { return base.protocol + ref; }
    h = ref.indexOf("#");
    cut = h < 0 ? ref : ref.slice(0, h);
    tail = h < 0 ? "" : ref.slice(h);
    q = cut.indexOf("?");
    path = q < 0 ? cut : cut.slice(0, q);
    q = q < 0 ? "" : cut.slice(cut.indexOf("?"));
    if (base.opaque) { return ref.charAt(0) === "#" ? serialize(base).split("#")[0] + ref : null; }
    root = base.protocol + "//" + (base.username || base.password ? base.username + (base.password ? ":" + base.password : "") + "@" : "") + base.hostname + (base.port ? ":" + base.port : "");
    if (path === "") {
      return root + base.pathname + (q === "" && cut === "" ? base.search : q) + tail;
    }
    if (path.charAt(0) !== "/") {
      path = base.pathname.slice(0, base.pathname.lastIndexOf("/") + 1) + path;
    }
    return root + removeDots(path) + q + tail;
  }
  function URL(url, base) {
    if (arguments.length === 0) { throw typeError("Failed to construct URL: 1 argument required"); }
    var r = null, b = null;
    if (base !== undefined) {
      b = parseAbsolute(String(base instanceof URL ? base.href : base));
      if (!b) { throw typeError("Failed to construct URL: Invalid base URL"); }
    }
    var abs = resolveRef(String(url), b);
    r = abs ? parseAbsolute(abs) : null;
    if (!r) { throw typeError("Failed to construct URL: Invalid URL"); }
    hide(this, "@r", r);
    hide(this, "@sp", null);
  }
  URL.canParse = function (url, base) { try { new URL(url, base); return true; } catch (e) { return false; } };
  URL.createObjectURL = function (blob) {
    if (!(blob instanceof g.Blob)) { throw typeError("URL.createObjectURL: argument must be a Blob"); }
    return N.createObjectURL(blob["@bytes"], blob.type, g.location.origin);
  };
  URL.revokeObjectURL = function (u) { N.revokeObjectURL(String(u)); };
  function urlProp(name, get, set) {
    def(URL.prototype, name, { get: get, set: set, configurable: true, enumerable: true });
  }
  function simple(name) {
    urlProp(name, function () { return this["@r"][name]; }, function (v) { this["@r"][name] = String(v); });
  }
  ["username", "password", "hostname"].forEach(simple);
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
    this["@r"].pathname = escapeWith(v.charAt(0) === "/" ? v : "/" + v, badPath);
  });
  urlProp("search", function () { return this["@r"].search; }, function (v) {
    v = String(v);
    this["@r"].search = v === "" || v === "?" ? "" : escapeWith(v.charAt(0) === "?" ? v : "?" + v, badQuery);
    if (this["@sp"]) { this["@sp"]["@list"].splice(0, this["@sp"]["@list"].length, ...parseQuery(this["@r"].search)); }
  });
  urlProp("hash", function () { return this["@r"].hash; }, function (v) {
    v = String(v);
    this["@r"].hash = v === "" || v === "#" ? "" : escapeWith(v.charAt(0) === "#" ? v : "#" + v, badFragment);
  });
  urlProp("href", function () { return serialize(this["@r"]); }, function (v) {
    var r = parseAbsolute(String(v));
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
  hide(URL.prototype, "@sync", function (q) { this["@r"].search = q ? "?" + q : ""; });
  method(URL.prototype, "toString", function () { return this.href; });
  method(URL.prototype, "toJSON", function () { return this.href; });
  hide(g, "URL", URL);

  /* ---- Blob, File ---- */
  function concat(chunks) {
    var n = 0, i, out, at = 0;
    for (i = 0; i < chunks.length; i++) { n += chunks[i].length; }
    out = new Uint8Array(n);
    for (i = 0; i < chunks.length; i++) { out.set(chunks[i], at); at += chunks[i].length; }
    return out;
  }
  function partBytes(p) {
    if (p instanceof Blob) { return p["@bytes"]; }
    if (p instanceof ArrayBuffer || ArrayBuffer.isView(p)) { return new Uint8Array(L.bytesOf(p)); }
    return N.utf8encode(String(p));
  }
  function Blob(parts, opts) {
    var chunks = [], i;
    if (parts !== undefined && parts !== null) {
      if (typeof parts !== "object" || typeof parts[Symbol.iterator] !== "function") { throw typeError("Failed to construct Blob: the parts must be a sequence"); }
      for (var p of parts) { chunks.push(partBytes(p)); }
    }
    hide(this, "@bytes", concat(chunks));
    this.type = opts && opts.type !== undefined ? String(opts.type).toLowerCase() : "";
  }
  def(Blob.prototype, "size", { get: function () { return this["@bytes"].length; }, configurable: true, enumerable: true });
  method(Blob.prototype, "slice", function (start, end, type) {
    var b = new Blob([], { type: type === undefined ? "" : type }), n = this["@bytes"].length;
    start = start === undefined ? 0 : start < 0 ? Math.max(n + start, 0) : Math.min(start, n);
    end = end === undefined ? n : end < 0 ? Math.max(n + end, 0) : Math.min(end, n);
    b["@bytes"] = this["@bytes"].slice(start, Math.max(start, end));
    return b;
  });
  method(Blob.prototype, "arrayBuffer", function () { return Promise.resolve(this["@bytes"].slice().buffer); });
  method(Blob.prototype, "bytes", function () { return Promise.resolve(this["@bytes"].slice()); });
  method(Blob.prototype, "text", function () { return Promise.resolve(N.decode(this["@bytes"], "utf-8", false, false)); });
  method(Blob.prototype, "toString", function () { return "[object Blob]"; });
  hide(g, "Blob", Blob);
  function File(parts, name, opts) {
    Blob.call(this, parts, opts);
    this.name = String(name);
    this.lastModified = opts && opts.lastModified !== undefined ? opts.lastModified : Date.now();
  }
  File.prototype = Object.create(Blob.prototype);
  hide(File.prototype, "constructor", File);
  hide(g, "File", File);

  /* ---- FormData ---- */
  function FormData() { hide(this, "@list", []); }
  function fdValue(v, filename) {
    if (v instanceof Blob) {
      if (!(v instanceof File) || filename !== undefined) { v = new File([v], filename === undefined ? "blob" : filename, { type: v.type }); }
      return v;
    }
    return String(v);
  }
  method(FormData.prototype, "append", function (k, v, f) { this["@list"].push([String(k), fdValue(v, f)]); });
  method(FormData.prototype, "set", function (k, v, f) {
    var l = this["@list"], i, done = false, out = [];
    k = String(k);
    for (i = 0; i < l.length; i++) {
      if (l[i][0] === k) { if (!done) { out.push([k, fdValue(v, f)]); done = true; } } else { out.push(l[i]); }
    }
    if (!done) { out.push([k, fdValue(v, f)]); }
    l.splice(0, l.length, ...out);
  });
  method(FormData.prototype, "delete", function (k) {
    k = String(k);
    this["@list"].splice(0, this["@list"].length, ...this["@list"].filter(function (p) { return p[0] !== k; }));
  });
  method(FormData.prototype, "get", function (k) {
    k = String(k);
    for (var i = 0; i < this["@list"].length; i++) { if (this["@list"][i][0] === k) { return this["@list"][i][1]; } }
    return null;
  });
  method(FormData.prototype, "getAll", function (k) {
    k = String(k);
    return this["@list"].filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; });
  });
  method(FormData.prototype, "has", function (k) { return this.get(k) !== null; });
  method(FormData.prototype, "forEach", function (fn, thisArg) { this["@list"].slice().forEach(function (p) { fn.call(thisArg, p[1], p[0], this); }, this); });
  method(FormData.prototype, "entries", function () { return this["@list"].map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); });
  method(FormData.prototype, "keys", function () { return this["@list"].map(function (p) { return p[0]; })[Symbol.iterator](); });
  method(FormData.prototype, "values", function () { return this["@list"].map(function (p) { return p[1]; })[Symbol.iterator](); });
  hide(FormData.prototype, Symbol.iterator, FormData.prototype.entries);
  hide(g, "FormData", FormData);

  /* ---- Headers ---- */
  function normName(n) {
    n = String(n);
    if (!n || n.search(/[^!#$%&*+.^_`|~0-9A-Za-z-]/) >= 0) { throw typeError("Invalid header name"); }
    return n.toLowerCase();
  }
  function Headers(init) {
    hide(this, "@map", {});
    hide(this, "@order", []);
    if (init === undefined || init === null) { return; }
    if (init instanceof Headers) { init.forEach(function (v, k) { this.append(k, v); }, this); }
    else if (typeof init === "object" && typeof init[Symbol.iterator] === "function") {
      for (var pair of init) {
        if (pair.length !== 2) { throw typeError("Headers: each header must be a [name, value] pair"); }
        this.append(pair[0], pair[1]);
      }
    } else if (typeof init === "object") {
      for (var k in init) { this.append(k, init[k]); }
    }
  }
  method(Headers.prototype, "append", function (n, v) {
    var k = normName(n), m = this["@map"];
    v = String(v).trim();
    if (m[k] === undefined) { m[k] = v; this["@order"].push(k); } else { m[k] += ", " + v; }
  });
  method(Headers.prototype, "set", function (n, v) {
    var k = normName(n);
    if (this["@map"][k] === undefined) { this["@order"].push(k); }
    this["@map"][k] = String(v).trim();
  });
  method(Headers.prototype, "get", function (n) { var v = this["@map"][normName(n)]; return v === undefined ? null : v; });
  method(Headers.prototype, "has", function (n) { return this["@map"][normName(n)] !== undefined; });
  method(Headers.prototype, "delete", function (n) {
    var k = normName(n), i = this["@order"].indexOf(k);
    if (i >= 0) { this["@order"].splice(i, 1); delete this["@map"][k]; }
  });
  method(Headers.prototype, "forEach", function (fn, thisArg) {
    var self = this;
    this["@order"].slice().sort().forEach(function (k) { fn.call(thisArg, self["@map"][k], k, self); });
  });
  method(Headers.prototype, "entries", function () {
    var self = this;
    return this["@order"].slice().sort().map(function (k) { return [k, self["@map"][k]]; })[Symbol.iterator]();
  });
  method(Headers.prototype, "keys", function () { return this["@order"].slice().sort()[Symbol.iterator](); });
  method(Headers.prototype, "values", function () {
    var self = this;
    return this["@order"].slice().sort().map(function (k) { return self["@map"][k]; })[Symbol.iterator]();
  });
  hide(Headers.prototype, Symbol.iterator, Headers.prototype.entries);
  hide(g, "Headers", Headers);
  L.formEncode = formEncode;
  L.concat = concat;
  L.partBytes = partBytes;
  L.Blob = Blob;
  L.Headers = Headers;
  L.URLSearchParams = URLSearchParams;
  L.FormData = FormData;
  L.File = File;
})
)
