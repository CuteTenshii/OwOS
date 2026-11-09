/* The web platform a dedicated worker sees, in JavaScript, stringified by the preprocessor (see webworker.c).
 * Keep it free of single quotes, backslashes and // comments, and end every statement with a semicolon: the line
 * breaks are lost. This file is not run through clang-format, which would split operators such as === apart.
 * It is called once per worker as f(globalThis, N, L): N holds the natives webworker.c provides, L is a scratch
 * object shared by the prelude parts (events, url, net, ...), which run in that order. */
JS(
(function (g, N, L) {
  var def = Object.defineProperty;
  function hide(o, k, v) { def(o, k, { value: v, writable: true, configurable: true, enumerable: false }); }
  function method(proto, name, fn) { hide(proto, name, fn); }
  function typeError(msg) { return new TypeError(msg); }

  /* ---- DOMException, events ---- */
  var DOM_CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5,
    NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
    InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15, TypeMismatchError: 17,
    SecurityError: 18, NetworkError: 19, AbortError: 20, URLMismatchError: 21, QuotaExceededError: 22,
    TimeoutError: 23, InvalidNodeTypeError: 24, DataCloneError: 25 };
  function DOMException(message, name) {
    var e = new Error(message === undefined ? "" : String(message));
    hide(e, "name", name === undefined ? "Error" : String(name));
    hide(e, "code", DOM_CODES[e.name] || 0);
    Object.setPrototypeOf(e, DOMException.prototype);
    return e;
  }
  DOMException.prototype = Object.create(Error.prototype);
  hide(DOMException.prototype, "constructor", DOMException);
  hide(g, "DOMException", DOMException);

  /* The defaults live on the prototype, so making an event costs two own properties plus whatever init sets */
  function Event(type, init) {
    if (arguments.length === 0) { throw typeError("Failed to construct Event: 1 argument required"); }
    this.type = String(type);
    this.timeStamp = g.performance.now();
    if (init) {
      if (init.bubbles) { this.bubbles = true; }
      if (init.cancelable) { this.cancelable = true; }
      if (init.composed) { this.composed = true; }
    }
  }
  [["bubbles", false], ["cancelable", false], ["composed", false], ["defaultPrevented", false], ["eventPhase", 0],
    ["isTrusted", false], ["target", null], ["currentTarget", null], ["@stop", false]].forEach(function (d) { hide(Event.prototype, d[0], d[1]); });
  method(Event.prototype, "preventDefault", function () { if (this.cancelable) { this.defaultPrevented = true; } });
  method(Event.prototype, "stopPropagation", function () {});
  method(Event.prototype, "stopImmediatePropagation", function () { this["@stop"] = true; });
  hide(g, "Event", Event);

  function subclass(name, fields) {
    function Sub(type, init) {
      Event.call(this, type, init);
      if (init) { for (var k in fields) { if (init[k] !== undefined) { this[k] = init[k]; } } }
    }
    Sub.prototype = Object.create(Event.prototype);
    hide(Sub.prototype, "constructor", Sub);
    for (var k in fields) { hide(Sub.prototype, k, fields[k]); }
    hide(g, name, Sub);
    return Sub;
  }
  subclass("CustomEvent", { detail: null });
  subclass("MessageEvent", { data: null, origin: "", lastEventId: "", source: null, ports: [] });
  subclass("ErrorEvent", { message: "", filename: "", lineno: 0, colno: 0, error: null });

  function listeners(t) {
    var l = t["@listeners"];
    if (!l) { l = {}; hide(t, "@listeners", l); }
    return l;
  }
  function EventTarget() {}
  method(EventTarget.prototype, "addEventListener", function (type, fn, opts) {
    if (typeof fn !== "function" && !(fn && typeof fn.handleEvent === "function")) { return; }
    var l = listeners(this), a = l[type] || (l[type] = []), once = !!(opts && typeof opts === "object" && opts.once);
    for (var i = 0; i < a.length; i++) { if (a[i].fn === fn) { return; } }
    a.push({ fn: fn, once: once });
  });
  method(EventTarget.prototype, "removeEventListener", function (type, fn) {
    var a = listeners(this)[type];
    if (!a) { return; }
    for (var i = 0; i < a.length; i++) { if (a[i].fn === fn) { a.splice(i, 1); return; } }
  });
  function runListeners(target, ev) {
    var a = listeners(target)[ev.type], i, rec;
    if (a) {
      a = a.slice();
      for (i = 0; i < a.length && !ev["@stop"]; i++) {
        rec = a[i];
        if (rec.once) { target.removeEventListener(ev.type, rec.fn); }
        if (typeof rec.fn === "function") { rec.fn.call(target, ev); } else { rec.fn.handleEvent(ev); }
      }
    }
  }
  method(EventTarget.prototype, "dispatchEvent", function (ev) {
    var h = this["on" + ev.type];
    ev.target = this;
    ev.currentTarget = this;
    if (typeof h === "function") { h.call(this, ev); }
    runListeners(this, ev);
    ev.currentTarget = null;
    return !ev.defaultPrevented;
  });
  hide(g, "EventTarget", EventTarget);
  ["addEventListener", "removeEventListener", "dispatchEvent"].forEach(function (k) { hide(g, k, EventTarget.prototype[k].bind(g)); });
  hide(g, "self", g);
  g.onmessage = null;
  g.onmessageerror = null;
  g.onerror = null;
  /* An uncaught error is offered to self.onerror (legacy signature) and error listeners before it reaches the page */
  N.onerror = function (message) {
    var handled = false, ev, h = g.onerror;
    if (typeof h === "function") { handled = h.call(g, message, g.location.href, 0, 0, null) === true; }
    ev = new g.ErrorEvent("error", { message: message, filename: g.location.href, cancelable: true });
    ev.target = g;
    runListeners(g, ev);
    return handled || ev.defaultPrevented;
  };

  /* ---- AbortController ---- */
  function AbortSignal() { throw typeError("Illegal constructor"); }
  AbortSignal.prototype = Object.create(EventTarget.prototype);
  hide(AbortSignal.prototype, "constructor", AbortSignal);
  function newSignal() {
    var s = Object.create(AbortSignal.prototype);
    s.aborted = false;
    s.reason = undefined;
    s.onabort = null;
    return s;
  }
  function abortSignal(s, reason) {
    if (s.aborted) { return; }
    s.aborted = true;
    s.reason = reason === undefined ? new DOMException("signal is aborted without reason", "AbortError") : reason;
    s.dispatchEvent(new Event("abort"));
  }
  method(AbortSignal.prototype, "throwIfAborted", function () { if (this.aborted) { throw this.reason; } });
  AbortSignal.abort = function (reason) { var s = newSignal(); abortSignal(s, reason); return s; };
  AbortSignal.timeout = function (ms) {
    var s = newSignal();
    g.setTimeout(function () { abortSignal(s, new DOMException("signal timed out", "TimeoutError")); }, ms);
    return s;
  };
  function AbortController() { hide(this, "@signal", newSignal()); }
  Object.defineProperty(AbortController.prototype, "signal", { get: function () { return this["@signal"]; }, configurable: true });
  method(AbortController.prototype, "abort", function (reason) { abortSignal(this["@signal"], reason); });
  hide(g, "AbortSignal", AbortSignal);
  hide(g, "AbortController", AbortController);

  /* ---- TextEncoder, TextDecoder ---- */
  function TextEncoder() {}
  Object.defineProperty(TextEncoder.prototype, "encoding", { get: function () { return "utf-8"; }, configurable: true });
  method(TextEncoder.prototype, "encode", function (s) { return N.utf8encode(s === undefined ? "" : String(s)); });
  method(TextEncoder.prototype, "encodeInto", function (s, dst) {
    var bytes = N.utf8encode(String(s)), n = Math.min(bytes.length, dst.length);
    dst.set(bytes.subarray(0, n));
    return { read: String(s).length, written: n };
  });
  hide(g, "TextEncoder", TextEncoder);

  function TextDecoder(label, opts) {
    var name = N.encodingName(label === undefined ? "utf-8" : String(label));
    if (!name) { throw new RangeError("The encoding label provided (" + label + ") is invalid."); }
    hide(this, "@label", name);
    hide(this, "@pending", null);
    this.encoding = name;
    this.fatal = !!(opts && opts.fatal);
    this.ignoreBOM = !!(opts && opts.ignoreBOM);
  }
  function bytesOf(input) {
    if (input === undefined) { return new Uint8Array(0); }
    if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer === "function" && input instanceof SharedArrayBuffer)) { return new Uint8Array(input); }
    if (ArrayBuffer.isView(input)) { return new Uint8Array(input.buffer, input.byteOffset, input.byteLength); }
    throw typeError("The provided value is not of type (ArrayBuffer or ArrayBufferView)");
  }
  method(TextDecoder.prototype, "decode", function (input, opts) {
    var bytes = bytesOf(input), stream = !!(opts && opts.stream), pend = this["@pending"], keep = 0, joined;
    if (pend && pend.length) {
      joined = new Uint8Array(pend.length + bytes.length);
      joined.set(pend);
      joined.set(bytes, pend.length);
      bytes = joined;
    }
    if (stream && this["@label"] === "utf-8") {
      /* hold back an incomplete trailing sequence until the next chunk */
      var n = bytes.length, i = n - 1, back = 0;
      while (i >= 0 && back < 3 && (bytes[i] & 192) === 128) { i--; back++; }
      if (i >= 0) {
        var lead = bytes[i], need = lead >= 240 ? 4 : lead >= 224 ? 3 : lead >= 192 ? 2 : 1;
        if (need > 1 && n - i < need) { keep = n - i; }
      }
    }
    this["@pending"] = keep ? bytes.slice(bytes.length - keep) : null;
    if (keep) { bytes = bytes.subarray(0, bytes.length - keep); }
    return N.decode(bytes, this["@label"], this.ignoreBOM, this.fatal);
  });
  hide(g, "TextDecoder", TextDecoder);

  /* ---- Streams: the classes are compiled the first time one is used ---- */
  var STREAM_NAMES = ["ReadableStream", "ReadableStreamDefaultReader", "ReadableStreamDefaultController", "WritableStream",
    "WritableStreamDefaultWriter", "WritableStreamDefaultController", "TransformStream", "TransformStreamDefaultController",
    "CountQueuingStrategy", "ByteLengthQueuingStrategy", "CompressionStream", "DecompressionStream", "TextEncoderStream", "TextDecoderStream"];
  var streamExports = null, streamsLoading = false;
  function loadStreams() {
    if (!streamExports && !streamsLoading) {
      streamsLoading = true;
      try { streamExports = N.streams(); } finally { streamsLoading = false; }
    }
    return streamExports;
  }
  STREAM_NAMES.forEach(function (name) {
    def(g, name, {
      get: function () {
        var ex = loadStreams(), v = ex ? ex[name] : undefined;
        if (ex) { def(g, name, { value: v, writable: true, configurable: true, enumerable: false }); }
        return v;
      },
      set: function (v) { def(g, name, { value: v, writable: true, configurable: true, enumerable: false }); },
      configurable: true
    });
  });
  L.loadStreams = loadStreams;

  /* ---- the rest of the platform is compiled on first use ---- */
  function lazyPart(part, names) {
    names.forEach(function (name) {
      def(g, name, {
        get: function () { N.loadPart(part); return g[name]; },
        set: function (v) { def(g, name, { value: v, writable: true, configurable: true, enumerable: false }); },
        configurable: true
      });
    });
  }
  lazyPart(1, ["URL", "URLSearchParams", "Blob", "File", "FormData", "FileReader", "Headers"]);
  lazyPart(2, ["Request", "Response", "fetch", "XMLHttpRequest", "WebSocket", "MessagePort", "MessageChannel", "Worker", "BroadcastChannel"]);

  /* ---- crypto ---- */
  var crypto = {};
  crypto.getRandomValues = function (view) {
    if (!ArrayBuffer.isView(view) || view instanceof Float32Array || view instanceof Float64Array) { throw new DOMException("The data argument must be an integer-type TypedArray", "TypeMismatchError"); }
    if (view.byteLength > 65536) { throw new DOMException("The ArrayBufferView byte length exceeds the number of bytes of entropy available via this API (65536)", "QuotaExceededError"); }
    N.randomFill(new Uint8Array(view.buffer, view.byteOffset, view.byteLength));
    return view;
  };
  crypto.randomUUID = function () {
    var b = new Uint8Array(16), h = "", i;
    N.randomFill(b);
    b[6] = (b[6] & 15) | 64;
    b[8] = (b[8] & 63) | 128;
    for (i = 0; i < 16; i++) {
      h += (b[i] < 16 ? "0" : "") + b[i].toString(16);
      if (i === 3 || i === 5 || i === 7 || i === 9) { h += "-"; }
    }
    return h;
  };
  hide(g, "crypto", crypto);

  /* ---- performance marks and measures ---- */
  var perf = g.performance, entries = [];
  perf.timeOrigin = Date.now() - perf.now();
  function entryTime(v, def) {
    if (v === undefined) { return def; }
    if (typeof v === "number") { return v; }
    for (var i = entries.length - 1; i >= 0; i--) { if (entries[i].entryType === "mark" && entries[i].name === v) { return entries[i].startTime; } }
    throw new DOMException("The mark " + QUOTE + v + QUOTE + " does not exist", "SyntaxError");
  }
  var QUOTE = String.fromCharCode(34);
  perf.mark = function (name, opts) {
    var e = { name: String(name), entryType: "mark", startTime: opts && opts.startTime !== undefined ? opts.startTime : perf.now(), duration: 0, detail: opts && opts.detail !== undefined ? opts.detail : null };
    entries.push(e);
    return e;
  };
  perf.measure = function (name, a, b) {
    var start, end, detail = null;
    if (a && typeof a === "object") { start = entryTime(a.start, 0); end = entryTime(a.end, a.duration !== undefined ? start + a.duration : perf.now()); detail = a.detail !== undefined ? a.detail : null; }
    else { start = entryTime(a, 0); end = entryTime(b, perf.now()); }
    var e = { name: String(name), entryType: "measure", startTime: start, duration: end - start, detail: detail };
    entries.push(e);
    return e;
  };
  perf.getEntries = function () { return entries.slice(); };
  perf.getEntriesByName = function (n, t) { return entries.filter(function (e) { return e.name === n && (t === undefined || e.entryType === t); }); };
  perf.getEntriesByType = function (t) { return entries.filter(function (e) { return e.entryType === t; }); };
  perf.clearMarks = function (n) { entries = entries.filter(function (e) { return e.entryType !== "mark" || (n !== undefined && e.name !== n); }); };
  perf.clearMeasures = function (n) { entries = entries.filter(function (e) { return e.entryType !== "measure" || (n !== undefined && e.name !== n); }); };
  perf.toJSON = function () { return { timeOrigin: perf.timeOrigin }; };

  hide(g, "navigator", { hardwareConcurrency: 4, onLine: true, language: "en-US", languages: ["en-US"], userAgent: "OwOS", platform: "OwOS" });
  L.hide = hide;
  L.method = method;
  L.typeError = typeError;
  L.EventTarget = EventTarget;
  L.Event = Event;
  L.DOMException = DOMException;
  L.abortSignal = abortSignal;
  L.newSignal = newSignal;
  L.bytesOf = bytesOf;
})
)
