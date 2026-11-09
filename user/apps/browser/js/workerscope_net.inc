/* Request, Response, fetch, XMLHttpRequest, WebSocket and MessageChannel for a dedicated worker; same conventions
 * as workerscope_prelude.inc (no single quotes, backslashes or // comments; every statement ends with a semicolon).
 * Network access is the page thread's: N.request posts {op, id, ...} and N.onreply gets {id, ok, ...} back. */
JS(
(function (g, N, L) {
  var hide = L.hide, method = L.method, typeError = L.typeError, def = Object.defineProperty;
  var Blob = L.Blob, Headers = L.Headers, URLSearchParams = L.URLSearchParams, FormData = L.FormData, File = L.File;
  var EventTarget = L.EventTarget, Event = L.Event, DOMException = L.DOMException;
  var CRLF = String.fromCharCode(13, 10), QUOTE = String.fromCharCode(34);
  var nextId = 0, pending = {};

  function request(payload, callback) {
    payload.id = ++nextId;
    pending[payload.id] = callback;
    N.request(payload);
    return payload.id;
  }
  N.onreply = function (r) {
    var cb = pending[r.id];
    if (cb) { delete pending[r.id]; cb(r); }
  };

  /* ---- bodies ---- */
  function multipart(fd) {
    var boundary = "----OwOSFormBoundary" + crypto.randomUUID().split("-").join(""), chunks = [], enc = N.utf8encode;
    fd["@list"].forEach(function (p) {
      var name = p[0].split(QUOTE).join("%22"), v = p[1];
      if (typeof v === "string") {
        chunks.push(enc("--" + boundary + CRLF + "Content-Disposition: form-data; name=" + QUOTE + name + QUOTE + CRLF + CRLF + v + CRLF));
      } else {
        chunks.push(enc("--" + boundary + CRLF + "Content-Disposition: form-data; name=" + QUOTE + name + QUOTE + "; filename=" + QUOTE + v.name.split(QUOTE).join("%22") + QUOTE + CRLF + "Content-Type: " + (v.type || "application/octet-stream") + CRLF + CRLF));
        chunks.push(v["@bytes"]);
        chunks.push(enc(CRLF));
      }
    });
    chunks.push(enc("--" + boundary + "--" + CRLF));
    return { bytes: L.concat(chunks), type: "multipart/form-data; boundary=" + boundary };
  }
  function extractBody(b) {
    if (b === null || b === undefined) { return { bytes: null, type: null }; }
    if (typeof b === "string") { return { bytes: N.utf8encode(b), type: "text/plain;charset=UTF-8" }; }
    if (b instanceof URLSearchParams) { return { bytes: N.utf8encode(b.toString()), type: "application/x-www-form-urlencoded;charset=UTF-8" }; }
    if (b instanceof Blob) { return { bytes: b["@bytes"].slice(), type: b.type || null }; }
    if (b instanceof FormData) { return multipart(b); }
    if (b instanceof ArrayBuffer || ArrayBuffer.isView(b)) { return { bytes: new Uint8Array(L.bytesOf(b)).slice(), type: null }; }
    return { bytes: N.utf8encode(String(b)), type: "text/plain;charset=UTF-8" };
  }
  function consume(self) {
    if (self["@used"]) { return Promise.reject(typeError("Body has already been consumed")); }
    self["@used"] = true;
    return Promise.resolve(self["@body"] || new Uint8Array(0));
  }
  function bodyMixin(proto) {
    method(proto, "arrayBuffer", function () { return consume(this).then(function (b) { return b.slice().buffer; }); });
    method(proto, "bytes", function () { return consume(this).then(function (b) { return b.slice(); }); });
    method(proto, "text", function () { return consume(this).then(function (b) { return N.decode(b, "utf-8", false, false); }); });
    method(proto, "json", function () { return this.text().then(function (t) { return JSON.parse(t); }); });
    method(proto, "blob", function () {
      var type = this.headers.get("content-type") || "";
      return consume(this).then(function (b) { return new Blob([b], { type: type }); });
    });
    method(proto, "formData", function () {
      var type = this.headers.get("content-type") || "";
      if (type.indexOf("application/x-www-form-urlencoded") !== 0) { return Promise.reject(typeError("Could not parse content as FormData.")); }
      return this.text().then(function (t) {
        var fd = new FormData();
        new URLSearchParams(t).forEach(function (v, k) { fd.append(k, v); });
        return fd;
      });
    });
    def(proto, "bodyUsed", { get: function () { return !!this["@used"]; }, configurable: true });
  }

  /* ---- Request ---- */
  function Request(input, init) {
    init = init || {};
    var src = input instanceof Request ? input : null, body, b, m;
    hide(this, "@used", false);
    this.url = new g.URL(src ? src.url : String(input), g.location.href).href;
    m = String(init.method !== undefined ? init.method : src ? src.method : "GET").toUpperCase();
    this.method = m;
    this.headers = new Headers(init.headers !== undefined ? init.headers : src ? src.headers : undefined);
    this.signal = init.signal || (src ? src.signal : L.newSignal());
    this.credentials = init.credentials || (src ? src.credentials : "same-origin");
    this.mode = init.mode || (src ? src.mode : "cors");
    this.cache = init.cache || "default";
    this.redirect = init.redirect || "follow";
    this.referrer = "about:client";
    this.referrerPolicy = init.referrerPolicy || "";
    this.integrity = init.integrity || "";
    this.keepalive = !!init.keepalive;
    body = init.body !== undefined ? init.body : null;
    if (body === null && src && !src["@used"]) { hide(this, "@body", src["@body"]); }
    else {
      b = extractBody(body);
      hide(this, "@body", b.bytes);
      if (b.type && !this.headers.has("content-type")) { this.headers.set("content-type", b.type); }
    }
    if (this["@body"] && (m === "GET" || m === "HEAD")) { throw typeError("Request with GET/HEAD method cannot have body."); }
  }
  bodyMixin(Request.prototype);
  method(Request.prototype, "clone", function () {
    if (this["@used"]) { throw typeError("Cannot clone a Request whose body is used"); }
    return new Request(this);
  });
  def(Request.prototype, "body", { get: function () { return null; }, configurable: true });
  hide(g, "Request", Request);

  /* ---- Response ---- */
  function Response(body, init) {
    init = init || {};
    var b = extractBody(body), status = init.status === undefined ? 200 : init.status;
    if (status < 200 || status > 599) { throw new RangeError("The status provided (" + status + ") is outside the range [200, 599]."); }
    hide(this, "@used", false);
    hide(this, "@body", b.bytes);
    this.status = status;
    this.statusText = init.statusText === undefined ? "" : String(init.statusText);
    this.headers = new Headers(init.headers);
    this.ok = status >= 200 && status < 300;
    this.url = "";
    this.redirected = false;
    this.type = "default";
    if (b.type && !this.headers.has("content-type")) { this.headers.set("content-type", b.type); }
  }
  bodyMixin(Response.prototype);
  method(Response.prototype, "clone", function () {
    if (this["@used"]) { throw typeError("Cannot clone a Response whose body is used"); }
    var r = new Response(this["@body"] ? this["@body"].slice() : null, { status: this.status, statusText: this.statusText, headers: this.headers });
    r.url = this.url;
    r.type = this.type;
    return r;
  });
  def(Response.prototype, "body", { get: function () { return null; }, configurable: true });
  Response.error = function () { var r = new Response(null, { status: 200 }); r.status = 0; r.ok = false; r.type = "error"; return r; };
  Response.redirect = function (url, status) {
    var r = new Response(null, { status: 200, headers: { location: new g.URL(String(url), g.location.href).href } });
    r.status = status === undefined ? 302 : status;
    r.ok = false;
    return r;
  };
  Response.json = function (data, init) {
    var r = new Response(JSON.stringify(data), init);
    r.headers.set("content-type", "application/json");
    return r;
  };
  hide(g, "Response", Response);

  function responseFrom(r, url) {
    var res = new Response(r.body.byteLength ? new Uint8Array(r.body) : null, { status: r.status < 200 ? 200 : r.status, statusText: r.statusText, headers: r.headers });
    res.status = r.status;
    res.ok = r.status >= 200 && r.status < 300;
    res.url = r.url || url;
    res.redirected = !!r.url && r.url !== url;
    res.type = "basic";
    return res;
  }

  /* ---- fetch ---- */
  hide(g, "fetch", function (input, init) {
    return new Promise(function (resolve, reject) {
      var req, sig, hs = [], id, onabort;
      try { req = new Request(input, init); } catch (e) { reject(e); return; }
      sig = req.signal;
      if (sig.aborted) { reject(sig.reason); return; }
      req.headers.forEach(function (v, k) { hs.push([k, v]); });
      id = request({ op: "fetch", url: req.url, method: req.method, headers: hs, body: req["@body"] ? req["@body"].slice().buffer : null, credentials: req.credentials }, function (r) {
        sig.removeEventListener("abort", onabort);
        if (!r.ok) { reject(typeError(r.error)); } else { resolve(responseFrom(r, req.url)); }
      });
      onabort = function () { delete pending[id]; reject(sig.reason); };
      sig.addEventListener("abort", onabort);
    });
  });

  /* ---- XMLHttpRequest ---- */
  var XHR_STATES = { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 };
  function XMLHttpRequest() {
    EventTarget.call(this);
    this.readyState = 0;
    this.status = 0;
    this.statusText = "";
    this.responseURL = "";
    this.responseType = "";
    this.timeout = 0;
    this.withCredentials = false;
    this.upload = new EventTarget();
    hide(this, "@req", null);
    hide(this, "@res", null);
    hide(this, "@headers", {});
    hide(this, "@id", 0);
    hide(this, "@sent", false);
  }
  XMLHttpRequest.prototype = Object.create(EventTarget.prototype);
  hide(XMLHttpRequest.prototype, "constructor", XMLHttpRequest);
  Object.keys(XHR_STATES).forEach(function (k) { hide(XMLHttpRequest, k, XHR_STATES[k]); hide(XMLHttpRequest.prototype, k, XHR_STATES[k]); });
  function xhrEvent(x, type, init) {
    var e = new Event(type);
    if (init) { for (var k in init) { e[k] = init[k]; } }
    x.dispatchEvent(e);
  }
  function xhrState(x, s) { x.readyState = s; xhrEvent(x, "readystatechange"); }
  method(XMLHttpRequest.prototype, "open", function (m, url, async, user, pass) {
    this["@req"] = { method: String(m).toUpperCase(), url: new g.URL(String(url), g.location.href).href, async: async === undefined ? true : !!async };
    this["@headers"] = {};
    this["@res"] = null;
    this["@sent"] = false;
    this.status = 0;
    this.statusText = "";
    xhrState(this, 1);
  });
  method(XMLHttpRequest.prototype, "setRequestHeader", function (k, v) {
    if (this.readyState !== 1 || this["@sent"]) { throw new DOMException("The object is in an invalid state.", "InvalidStateError"); }
    var key = String(k).toLowerCase();
    this["@headers"][key] = this["@headers"][key] === undefined ? String(v) : this["@headers"][key] + ", " + String(v);
  });
  function xhrFinish(x, r, failure) {
    if (x["@id"] === 0) { return; }
    x["@id"] = 0;
    if (x["@timer"]) { g.clearTimeout(x["@timer"]); x["@timer"] = 0; }
    if (!r || !r.ok) {
      x.status = 0;
      xhrState(x, 4);
      xhrEvent(x, failure || "error");
      xhrEvent(x, "loadend");
      return;
    }
    x["@res"] = r;
    x.status = r.status;
    x.statusText = r.statusText;
    x.responseURL = r.url;
    xhrState(x, 2);
    xhrState(x, 3);
    xhrState(x, 4);
    xhrEvent(x, "progress", { loaded: r.body.byteLength, total: r.body.byteLength, lengthComputable: true });
    xhrEvent(x, "load", { loaded: r.body.byteLength, total: r.body.byteLength, lengthComputable: true });
    xhrEvent(x, "loadend");
  }
  method(XMLHttpRequest.prototype, "send", function (body) {
    var q = this["@req"], hs = [], b, self = this, k, payload, id;
    if (this.readyState !== 1 || this["@sent"] || !q) { throw new DOMException("The object is in an invalid state.", "InvalidStateError"); }
    this["@sent"] = true;
    b = extractBody(body);
    for (k in this["@headers"]) { hs.push([k, this["@headers"][k]]); }
    if (b.type && this["@headers"]["content-type"] === undefined) { hs.push(["content-type", b.type]); }
    payload = { op: "fetch", url: q.url, method: q.method, headers: hs, body: b.bytes ? b.bytes.buffer : null, credentials: this.withCredentials ? "include" : "same-origin" };
    xhrEvent(this, "loadstart");
    if (!q.async) {
      payload.id = ++nextId;
      this["@id"] = payload.id;
      N.request(payload);
      xhrFinish(this, N.waitReply(payload.id), "error");
      if (this.status === 0) { throw new DOMException("Failed to execute send on XMLHttpRequest: network error", "NetworkError"); }
      return;
    }
    id = request(payload, function (r) { xhrFinish(self, r, "error"); });
    this["@id"] = id;
    if (this.timeout > 0) {
      this["@timer"] = g.setTimeout(function () { delete pending[id]; xhrFinish(self, null, "timeout"); }, this.timeout);
    }
  });
  method(XMLHttpRequest.prototype, "abort", function () {
    var id = this["@id"];
    if (id) {
      delete pending[id];
      this["@id"] = 0;
      if (this["@timer"]) { g.clearTimeout(this["@timer"]); }
      this.status = 0;
      xhrState(this, 4);
      xhrEvent(this, "abort");
      xhrEvent(this, "loadend");
    }
    this.readyState = 0;
  });
  method(XMLHttpRequest.prototype, "getResponseHeader", function (n) {
    var r = this["@res"], k = String(n).toLowerCase(), out = null;
    if (!r) { return null; }
    r.headers.forEach(function (p) { if (p[0].toLowerCase() === k) { out = out === null ? p[1] : out + ", " + p[1]; } });
    return out;
  });
  method(XMLHttpRequest.prototype, "getAllResponseHeaders", function () {
    var r = this["@res"], out = "";
    if (!r) { return ""; }
    r.headers.forEach(function (p) { out += p[0].toLowerCase() + ": " + p[1] + CRLF; });
    return out;
  });
  method(XMLHttpRequest.prototype, "overrideMimeType", function () {});
  def(XMLHttpRequest.prototype, "responseText", { get: function () {
    if (this.responseType !== "" && this.responseType !== "text") { throw new DOMException("The value is only accessible if the object responseType is empty or text", "InvalidStateError"); }
    return this["@res"] ? N.decode(new Uint8Array(this["@res"].body), "utf-8", false, false) : "";
  }, configurable: true });
  def(XMLHttpRequest.prototype, "response", { get: function () {
    var r = this["@res"], t = this.responseType, type;
    if (!r || this.readyState !== 4) { return t === "" || t === "text" ? "" : null; }
    if (t === "arraybuffer") { return r.body.slice(0); }
    if (t === "blob") {
      type = "";
      r.headers.forEach(function (p) { if (p[0].toLowerCase() === "content-type") { type = p[1]; } });
      return new Blob([new Uint8Array(r.body)], { type: type });
    }
    if (t === "json") { try { return JSON.parse(N.decode(new Uint8Array(r.body), "utf-8", false, false)); } catch (e) { return null; } }
    return N.decode(new Uint8Array(r.body), "utf-8", false, false);
  }, configurable: true });
  hide(g, "XMLHttpRequest", XMLHttpRequest);

  /* ---- WebSocket ---- */
  var sockets = {};
  function WebSocket(url, protocols) {
    EventTarget.call(this);
    var u = new g.URL(String(url), g.location.href), self = this, list = "", reply, handle;
    if (u.protocol === "http:") { u.protocol = "ws:"; } else if (u.protocol === "https:") { u.protocol = "wss:"; }
    if (u.protocol !== "ws:" && u.protocol !== "wss:") { throw new DOMException("The URL " + QUOTE + url + QUOTE + " is invalid.", "SyntaxError"); }
    if (u.hash) { throw new DOMException("The URL contains a fragment identifier.", "SyntaxError"); }
    if (protocols !== undefined) { list = typeof protocols === "string" ? protocols : Array.prototype.join.call(protocols, ","); }
    this.url = u.href;
    this.readyState = 0;
    this.bufferedAmount = 0;
    this.extensions = "";
    this.protocol = "";
    this.binaryType = "blob";
    this.onopen = null;
    this.onmessage = null;
    this.onclose = null;
    this.onerror = null;
    hide(this, "@h", -1);
    reply = (function () { var id = ++nextId; N.request({ op: "connect", id: id, url: u.href }); return N.waitReply(id); })();
    handle = reply && reply.ok ? N.wsOpen(u.href, list) : -1;
    g.setTimeout(function () {
      if (self.readyState !== 0) { return; }
      if (handle < 0) {
        self.readyState = 3;
        xhrEvent(self, "error");
        xhrEvent(self, "close", { code: 1006, reason: "", wasClean: false });
        return;
      }
      self["@h"] = handle;
      sockets[handle] = self;
      self.readyState = 1;
      self.protocol = N.wsProtocol(handle);
      xhrEvent(self, "open");
    }, 0);
  }
  WebSocket.prototype = Object.create(EventTarget.prototype);
  hide(WebSocket.prototype, "constructor", WebSocket);
  [["CONNECTING", 0], ["OPEN", 1], ["CLOSING", 2], ["CLOSED", 3]].forEach(function (p) { hide(WebSocket, p[0], p[1]); hide(WebSocket.prototype, p[0], p[1]); });
  method(WebSocket.prototype, "send", function (data) {
    if (this.readyState === 0) { throw new DOMException("Still in CONNECTING state.", "InvalidStateError"); }
    if (this.readyState !== 1) { return; }
    if (data instanceof Blob) { data = data["@bytes"]; }
    else if (typeof data !== "string" && !(data instanceof ArrayBuffer) && !ArrayBuffer.isView(data)) { data = String(data); }
    N.wsSend(this["@h"], data);
  });
  method(WebSocket.prototype, "close", function (code, reason) {
    if (code !== undefined && code !== 1000 && (code < 3000 || code > 4999)) { throw new DOMException("The close code must be either 1000, or between 3000 and 4999.", "InvalidAccessError"); }
    if (this.readyState >= 2) { return; }
    this.readyState = 2;
    if (this["@h"] >= 0) { N.wsClose(this["@h"], code === undefined ? 1000 : code, reason === undefined ? "" : String(reason)); }
  });
  N.onws = function (h, type, data) {
    var s = sockets[h], v;
    if (!s) { return; }
    if (type === "message") {
      v = data;
      if (typeof data !== "string" && s.binaryType === "blob") { v = new Blob([data]); }
      xhrEvent(s, "message", { data: v, origin: "", lastEventId: "", source: null, ports: [] });
      return;
    }
    delete sockets[h];
    s.readyState = 3;
    if (type === "error") { xhrEvent(s, "error"); }
    xhrEvent(s, "close", { code: type === "error" ? 1006 : 1005, reason: "", wasClean: type !== "error" });
  };
  hide(g, "WebSocket", WebSocket);

  /* ---- MessageChannel: ports in this worker only ---- */
  function MessagePort() { throw typeError("Illegal constructor"); }
  MessagePort.prototype = Object.create(EventTarget.prototype);
  hide(MessagePort.prototype, "constructor", MessagePort);
  function newPort() {
    var p = Object.create(MessagePort.prototype);
    hide(p, "@queue", []);
    hide(p, "@peer", null);
    hide(p, "@started", false);
    hide(p, "@scheduled", false);
    p.onmessageerror = null;
    return p;
  }
  function flush(p) {
    p["@scheduled"] = false;
    while (p["@started"] && p["@queue"].length) {
      p.dispatchEvent(new g.MessageEvent("message", { data: p["@queue"].shift(), target: p }));
    }
  }
  function schedule(p) {
    if (p["@scheduled"] || !p["@started"]) { return; }
    p["@scheduled"] = true;
    g.setTimeout(function () { flush(p); }, 0);
  }
  method(MessagePort.prototype, "postMessage", function (data) {
    var peer = this["@peer"];
    if (!peer) { return; }
    peer["@queue"].push(structuredClone(data));
    schedule(peer);
  });
  method(MessagePort.prototype, "start", function () { this["@started"] = true; schedule(this); });
  method(MessagePort.prototype, "close", function () {
    var peer = this["@peer"];
    this["@peer"] = null;
    if (peer) { peer["@peer"] = null; }
  });
  function MessageChannel() {
    var a = newPort(), b = newPort();
    a["@peer"] = b;
    b["@peer"] = a;
    this.port1 = a;
    this.port2 = b;
  }
  hide(g, "MessagePort", MessagePort);
  hide(g, "MessageChannel", MessageChannel);
  /* assigning onmessage starts a port, as in browsers */
  def(MessagePort.prototype, "onmessage", {
    get: function () { return this["@onmessage"] || null; },
    set: function (fn) { hide(this, "@onmessage", fn); if (typeof fn === "function") { this["@started"] = true; schedule(this); } },
    configurable: true
  });
})
)
