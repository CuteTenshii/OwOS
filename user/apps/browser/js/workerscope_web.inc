/* URL, URLSearchParams, Blob, File, FormData and Headers for a dedicated worker; same conventions as
 * workerscope_prelude.inc (no single quotes, backslashes or // comments; every statement ends with a semicolon). */
JS(
(function (g, N, L) {
  var hide = L.hide, method = L.method, typeError = L.typeError, def = Object.defineProperty;
  var QUOTE = String.fromCharCode(39);

  /* ---- percent coding ---- */
  /* ---- URL, URLSearchParams: shared with the page, see url.inc ---- */
  var urlClasses = N.urlClasses(), URL = urlClasses.URL, URLSearchParams = urlClasses.URLSearchParams;
  URL.createObjectURL = function (blob) {
    if (!(blob instanceof g.Blob)) { throw typeError("URL.createObjectURL: argument must be a Blob"); }
    return N.createObjectURL(blob["@bytes"], blob.type, g.location.origin);
  };
  URL.revokeObjectURL = function (u) { N.revokeObjectURL(String(u)); };

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

  /* ---- Headers: shared with the page, see headers.inc ---- */
  var Headers = N.headers();
  L.concat = concat;
  L.partBytes = partBytes;
  L.Blob = Blob;
  L.Headers = Headers;
  L.URLSearchParams = URLSearchParams;
  L.FormData = FormData;
  L.File = File;
})
)
