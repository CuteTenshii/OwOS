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

  function Event(type, init) {
    if (arguments.length === 0) { throw typeError("Failed to construct Event: 1 argument required"); }
    init = init || {};
    hide(this, "@stop", false);
    this.type = String(type);
    this.bubbles = !!init.bubbles;
    this.cancelable = !!init.cancelable;
    this.composed = !!init.composed;
    this.defaultPrevented = false;
    this.target = null;
    this.currentTarget = null;
    this.eventPhase = 0;
    this.isTrusted = false;
    this.timeStamp = g.performance.now();
  }
  method(Event.prototype, "preventDefault", function () { if (this.cancelable) { this.defaultPrevented = true; } });
  method(Event.prototype, "stopPropagation", function () {});
  method(Event.prototype, "stopImmediatePropagation", function () { this["@stop"] = true; });
  hide(g, "Event", Event);

  function subclass(name, fields) {
    function Sub(type, init) {
      Event.call(this, type, init);
      init = init || {};
      for (var k in fields) { this[k] = init[k] === undefined ? fields[k] : init[k]; }
    }
    Sub.prototype = Object.create(Event.prototype);
    hide(Sub.prototype, "constructor", Sub);
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
  method(EventTarget.prototype, "dispatchEvent", function (ev) {
    var h = this["on" + ev.type], a = listeners(this)[ev.type], i, rec;
    ev.target = this;
    ev.currentTarget = this;
    if (typeof h === "function") { h.call(this, ev); }
    if (a) {
      a = a.slice();
      for (i = 0; i < a.length && !ev["@stop"]; i++) {
        rec = a[i];
        if (rec.once) { this.removeEventListener(ev.type, rec.fn); }
        if (typeof rec.fn === "function") { rec.fn.call(this, ev); } else { rec.fn.handleEvent(ev); }
      }
    }
    ev.currentTarget = null;
    return !ev.defaultPrevented;
  });
  hide(g, "EventTarget", EventTarget);
  ["addEventListener", "removeEventListener", "dispatchEvent"].forEach(function (k) { hide(g, k, EventTarget.prototype[k]); });
  hide(g, "self", g);
  g.onmessage = null;
  g.onmessageerror = null;
  g.onerror = null;
  hide(g, "@deliver", function (data) { g.dispatchEvent(new g.MessageEvent("message", { data: data })); });

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
