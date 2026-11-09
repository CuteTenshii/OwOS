/* URL, URLSearchParams, Blob, File, FormData and Headers for a dedicated worker; same conventions as
 * workerscope_prelude.inc (no single quotes, backslashes or // comments; every statement ends with a semicolon). */
JS(
(function (g, N, L) {
  var hide = L.hide, method = L.method, typeError = L.typeError, def = Object.defineProperty;
  var QUOTE = String.fromCharCode(39);

  /* ---- percent coding ---- */
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
  method(Blob.prototype, "stream", function () { return L.loadStreams().bytesStream(this["@bytes"].slice()); });
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

  /* ---- FileReader ---- */
  function FileReader() {
    L.EventTarget.call(this);
    this.readyState = 0;
    this.result = null;
    this.error = null;
    this.onload = null;
    this.onerror = null;
    this.onloadend = null;
    this.onloadstart = null;
    this.onabort = null;
    this.onprogress = null;
    hide(this, "@token", 0);
  }
  FileReader.prototype = Object.create(L.EventTarget.prototype);
  hide(FileReader.prototype, "constructor", FileReader);
  [["EMPTY", 0], ["LOADING", 1], ["DONE", 2]].forEach(function (p) { hide(FileReader, p[0], p[1]); hide(FileReader.prototype, p[0], p[1]); });
  function frFire(r, type) {
    var e = new L.Event(type);
    e.loaded = r.result ? (r.result.byteLength !== undefined ? r.result.byteLength : r.result.length) : 0;
    e.total = e.loaded;
    r.dispatchEvent(e);
  }
  function frRead(r, blob, convert) {
    if (!(blob instanceof Blob)) { throw typeError("Failed to execute read on FileReader: parameter 1 is not of type Blob"); }
    if (r.readyState === 1) { throw new L.DOMException("The object is already busy reading Blobs.", "InvalidStateError"); }
    var token = ++r["@token"];
    r.readyState = 1;
    r.result = null;
    frFire(r, "loadstart");
    g.setTimeout(function () {
      if (r["@token"] !== token) { return; }
      r.result = convert(blob["@bytes"]);
      r.readyState = 2;
      frFire(r, "progress");
      frFire(r, "load");
      frFire(r, "loadend");
    }, 0);
  }
  method(FileReader.prototype, "readAsText", function (blob, enc) { frRead(this, blob, function (b) { return N.decode(b, enc === undefined ? "utf-8" : String(enc), false, false); }); });
  method(FileReader.prototype, "readAsArrayBuffer", function (blob) { frRead(this, blob, function (b) { return b.slice().buffer; }); });
  method(FileReader.prototype, "readAsBinaryString", function (blob) {
    frRead(this, blob, function (b) { return N.binaryString(b); });
  });
  method(FileReader.prototype, "readAsDataURL", function (blob) {
    frRead(this, blob, function (b) { return "data:" + (blob.type || "application/octet-stream") + ";base64," + N.base64(b); });
  });
  method(FileReader.prototype, "abort", function () {
    if (this.readyState !== 1) { return; }
    this["@token"]++;
    this.readyState = 2;
    this.result = null;
    frFire(this, "abort");
    frFire(this, "loadend");
  });
  hide(g, "FileReader", FileReader);

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
  var goodNames = {}, goodCount = 0;
  function normName(n) {
    n = String(n);
    var k = goodNames[n];
    if (k !== undefined) { return k; }
    if (!n || n.search(/[^!#$%&*+.^_`|~0-9A-Za-z-]/) >= 0) { throw typeError("Invalid header name"); }
    k = n.toLowerCase();
    if (goodCount < 512) { goodNames[n] = k; goodCount++; }
    return k;
  }
  function sortedNames(h) {
    var s = h["@sorted"];
    if (!s) { s = h["@order"].slice().sort(); h["@sorted"] = s; }
    return s;
  }
  function Headers(init) {
    hide(this, "@map", {});
    hide(this, "@order", []);
    hide(this, "@sorted", null);
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
    if (m[k] === undefined) { m[k] = v; this["@order"].push(k); this["@sorted"] = null; } else { m[k] += ", " + v; }
  });
  method(Headers.prototype, "set", function (n, v) {
    var k = normName(n);
    if (this["@map"][k] === undefined) { this["@order"].push(k); this["@sorted"] = null; }
    this["@map"][k] = String(v).trim();
  });
  method(Headers.prototype, "get", function (n) { var v = this["@map"][normName(n)]; return v === undefined ? null : v; });
  method(Headers.prototype, "has", function (n) { return this["@map"][normName(n)] !== undefined; });
  method(Headers.prototype, "delete", function (n) {
    var k = normName(n), i = this["@order"].indexOf(k);
    if (i >= 0) { this["@order"].splice(i, 1); delete this["@map"][k]; this["@sorted"] = null; }
  });
  method(Headers.prototype, "forEach", function (fn, thisArg) {
    var self = this;
    sortedNames(this).slice().forEach(function (k) { fn.call(thisArg, self["@map"][k], k, self); });
  });
  method(Headers.prototype, "entries", function () {
    var self = this;
    return sortedNames(this).map(function (k) { return [k, self["@map"][k]]; })[Symbol.iterator]();
  });
  method(Headers.prototype, "keys", function () { return sortedNames(this).slice()[Symbol.iterator](); });
  method(Headers.prototype, "values", function () {
    var self = this;
    return sortedNames(this).map(function (k) { return self["@map"][k]; })[Symbol.iterator]();
  });
  hide(Headers.prototype, Symbol.iterator, Headers.prototype.entries);
  hide(g, "Headers", Headers);
  L.concat = concat;
  L.partBytes = partBytes;
  L.Blob = Blob;
  L.Headers = Headers;
  L.URLSearchParams = URLSearchParams;
  L.FormData = FormData;
  L.File = File;
})
)
