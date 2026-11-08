/* The Streams API in JavaScript, stringified by the preprocessor (see streams.c). Keep it free of single quotes
 * and backslashes, and end every statement with a semicolon: the line breaks are lost. This file is not run through
 * clang-format, which would split operators such as === apart. */
JS(
(function (codec) {
  var S = Symbol("stream");
  var INTERNAL = {};
  var MAX_BUFFERED = 67108864;
  function noop() {}
  function isObj(v) { return (typeof v === "object" && v !== null) || typeof v === "function"; }
  function deferred() {
    var d = {};
    d.promise = new Promise(function (res, rej) { d.resolve = res; d.reject = rej; });
    return d;
  }
  var jobs = [], running = false;
  function run(job) {
    jobs.push(job);
    if (running) return;
    running = true;
    try {
      while (jobs.length) {
        try { jobs.shift()(); } catch (e) { noop(e); }
      }
    } finally { running = false; }
  }
  function after(p, onFulfilled, onRejected) {
    Promise.resolve(p).then(function (v) {
      run(function () { onFulfilled(v); });
    }, function (e) {
      run(function () { (onRejected || noop)(e); });
    });
  }
  function handled(p) { p.then(undefined, noop); return p; }
  function rejected(e) { return handled(Promise.reject(e)); }
  function brand(o, kind) {
    var s = o !== null && o !== undefined ? o[S] : undefined;
    if (!s || s.kind !== kind) throw new TypeError("Illegal invocation");
    return s;
  }
  function brandP(o, kind) {
    try { return brand(o, kind); } catch (e) { return e; }
  }
  function toHwm(v) {
    var n = Number(v);
    if (n !== n || n < 0) throw new RangeError("Invalid highWaterMark");
    return n;
  }
  function strategyHwm(strategy, dflt) {
    var v = isObj(strategy) ? strategy.highWaterMark : undefined;
    return v === undefined ? dflt : toHwm(v);
  }
  function strategySize(strategy) {
    var f = isObj(strategy) ? strategy.size : undefined;
    if (f === undefined) return function () { return 1; };
    if (typeof f !== "function") throw new TypeError("size must be a function");
    return f;
  }
  function chunkSize(fn, chunk) {
    var n = Number(fn(chunk));
    if (n !== n || n < 0 || n === Infinity) throw new RangeError("Invalid chunk size");
    return n;
  }
  function method(obj, name) {
    var f = obj[name];
    if (f === undefined || f === null) return undefined;
    if (typeof f !== "function") throw new TypeError(name + " must be a function");
    return f;
  }
  function toBytes(chunk) {
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk.slice(0));
    if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength).slice();
    throw new TypeError("Chunk must be an ArrayBuffer or a view");
  }

  function CountQueuingStrategy(init) {
    if (!isObj(init)) throw new TypeError("Argument must be an object");
    this.highWaterMark = toHwm(init.highWaterMark);
  }
  CountQueuingStrategy.prototype.size = function () { return 1; };
  function ByteLengthQueuingStrategy(init) {
    if (!isObj(init)) throw new TypeError("Argument must be an object");
    this.highWaterMark = toHwm(init.highWaterMark);
  }
  ByteLengthQueuingStrategy.prototype.size = function (chunk) { return chunk.byteLength; };

  function rsDesired(r) {
    if (r.state === "errored") return null;
    if (r.state === "closed") return 0;
    return r.hwm - r.qsize;
  }
  function rsShouldPull(r) {
    if (r.state !== "readable" || r.closeRequested || !r.started) return false;
    if (r.reader && r.reader.reqs.length > 0) return true;
    return rsDesired(r) > 0;
  }
  function rsPull(r) {
    if (!rsShouldPull(r)) return;
    if (r.pulling) { r.pullAgain = true; return; }
    if (!r.pullFn) return;
    r.pulling = true;
    var p;
    try { p = Promise.resolve(r.pullFn.call(r.src, r.ctrl)); } catch (e) { rsError(r, e); return; }
    after(p, function () {
      r.pulling = false;
      if (r.pullAgain) { r.pullAgain = false; rsPull(r); }
    }, function (e) { rsError(r, e); });
  }
  function rsError(r, e) {
    if (r.state !== "readable") return;
    r.state = "errored";
    r.err = e;
    r.queue = [];
    r.qsize = 0;
    var rd = r.reader;
    if (rd) {
      rd.closed.reject(e);
      handled(rd.closed.promise);
      var q = rd.reqs;
      rd.reqs = [];
      for (var i = 0; i < q.length; i++) q[i].reject(e);
    }
  }
  function rsClose(r) {
    if (r.state !== "readable") return;
    r.state = "closed";
    var rd = r.reader;
    if (rd) {
      rd.closed.resolve(undefined);
      var q = rd.reqs;
      rd.reqs = [];
      for (var i = 0; i < q.length; i++) q[i].resolve({ value: undefined, done: true });
    }
  }
  function rsCloseRequest(r) {
    if (r.closeRequested || r.state !== "readable") throw new TypeError("Stream is not closable");
    r.closeRequested = true;
    if (r.queue.length === 0) rsClose(r);
  }
  function rsEnqueue(r, chunk) {
    if (r.closeRequested || r.state !== "readable") throw new TypeError("Cannot enqueue: stream is closed or errored");
    if (r.bytes && !ArrayBuffer.isView(chunk)) throw new TypeError("Chunk must be an ArrayBuffer view");
    if (r.reader && r.reader.reqs.length > 0) {
      r.reader.reqs.shift().resolve({ value: chunk, done: false });
    } else {
      var size;
      try { size = chunkSize(r.sizeFn, chunk); } catch (e) { rsError(r, e); throw e; }
      r.queue.push({ chunk: chunk, size: size });
      r.qsize += size;
    }
    rsPull(r);
  }
  function rsRead(r, rd) {
    r.disturbed = true;
    if (r.state === "closed") return Promise.resolve({ value: undefined, done: true });
    if (r.state === "errored") return rejected(r.err);
    if (r.queue.length > 0) {
      var it = r.queue.shift();
      r.qsize -= it.size;
      if (r.qsize < 0) r.qsize = 0;
      if (r.closeRequested && r.queue.length === 0) rsClose(r); else rsPull(r);
      return Promise.resolve({ value: it.chunk, done: false });
    }
    var d = deferred();
    rd.reqs.push(d);
    rsPull(r);
    return d.promise;
  }
  function rsCancel(r, reason) {
    r.disturbed = true;
    if (r.state === "closed") return Promise.resolve(undefined);
    if (r.state === "errored") return rejected(r.err);
    r.queue = [];
    r.qsize = 0;
    rsClose(r);
    var res;
    try { res = r.cancelFn ? r.cancelFn.call(r.src, reason) : undefined; } catch (e) { return rejected(e); }
    return Promise.resolve(res).then(function () { return undefined; });
  }

  class ReadableStreamDefaultController {
    constructor(token, r) {
      if (token !== INTERNAL) throw new TypeError("Illegal constructor");
      this[S] = { kind: "rc", r: r };
    }
    get desiredSize() { return rsDesired(brand(this, "rc").r); }
    close() { rsCloseRequest(brand(this, "rc").r); }
    enqueue(chunk) { rsEnqueue(brand(this, "rc").r, chunk); }
    error(e) { rsError(brand(this, "rc").r, e); }
  }

  class ReadableStreamDefaultReader {
    constructor(stream) {
      var r = brand(stream, "rs");
      if (r.reader) throw new TypeError("ReadableStream is locked");
      var d = { kind: "rd", r: r, reqs: [], closed: deferred() };
      this[S] = d;
      r.reader = d;
      handled(d.closed.promise);
      if (r.state === "closed") d.closed.resolve(undefined);
      else if (r.state === "errored") d.closed.reject(r.err);
    }
    get closed() {
      var d = brandP(this, "rd");
      return d instanceof Error ? rejected(d) : d.closed.promise;
    }
    read() {
      var d = brandP(this, "rd");
      if (d instanceof Error) return rejected(d);
      if (!d.r) return rejected(new TypeError("Reader has been released"));
      return rsRead(d.r, d);
    }
    cancel(reason) {
      var d = brandP(this, "rd");
      if (d instanceof Error) return rejected(d);
      if (!d.r) return rejected(new TypeError("Reader has been released"));
      return rsCancel(d.r, reason);
    }
    releaseLock() {
      var d = brand(this, "rd");
      if (!d.r) return;
      var err = new TypeError("Reader has been released");
      var q = d.reqs;
      d.reqs = [];
      for (var i = 0; i < q.length; i++) q[i].reject(err);
      if (d.r.state === "readable") d.closed.reject(err);
      else { d.closed = deferred(); d.closed.reject(err); }
      handled(d.closed.promise);
      d.r.reader = null;
      d.r = null;
    }
  }

  function readableFrom(src) {
    var it;
    if (src !== null && src !== undefined && src[Symbol.asyncIterator]) it = src[Symbol.asyncIterator]();
    else if (src !== null && src !== undefined && src[Symbol.iterator]) it = src[Symbol.iterator]();
    else throw new TypeError("Argument is not iterable");
    return new ReadableStream({
      pull: function (c) {
        return Promise.resolve(it.next()).then(function (res) {
          if (!isObj(res)) throw new TypeError("Iterator result is not an object");
          if (res.done) { c.close(); return; }
          return Promise.resolve(res.value).then(function (v) { c.enqueue(v); });
        });
      },
      cancel: function (reason) {
        if (it["return"]) return it["return"](reason);
      }
    }, { highWaterMark: 0 });
  }

  function teeStream(stream) {
    var reader = stream.getReader();
    var reading = false, again = false, canceled1 = false, canceled2 = false, ready = false;
    var reason1, reason2, c1, c2, done = deferred();
    function pull() {
      if (!ready || reading) { again = true; return Promise.resolve(); }
      reading = true;
      after(reader.read(), function (res) {
        reading = false;
        if (res.done) {
          if (!canceled1) c1.close();
          if (!canceled2) c2.close();
          if (!canceled1 || !canceled2) done.resolve(undefined);
          return;
        }
        if (!canceled1) c1.enqueue(res.value);
        if (!canceled2) c2.enqueue(res.value);
        if (again) { again = false; pull(); }
      });
      return Promise.resolve();
    }
    function cancelBranch(which, reason) {
      if (which === 1) { canceled1 = true; reason1 = reason; } else { canceled2 = true; reason2 = reason; }
      if (canceled1 && canceled2) reader.cancel([reason1, reason2]).then(done.resolve, done.reject);
      return done.promise;
    }
    var b1 = new ReadableStream({
      start: function (c) { c1 = c; },
      pull: pull,
      cancel: function (reason) { return cancelBranch(1, reason); }
    });
    var b2 = new ReadableStream({
      start: function (c) { c2 = c; },
      pull: pull,
      cancel: function (reason) { return cancelBranch(2, reason); }
    });
    ready = true;
    after(reader.closed, noop, function (e) {
      c1.error(e);
      c2.error(e);
      if (!canceled1 || !canceled2) done.resolve(undefined);
    });
    if (again) { again = false; pull(); }
    return [b1, b2];
  }

  function pipe(source, dest, options) {
    var preventClose = false, preventAbort = false, preventCancel = false, signal;
    if (options !== undefined && options !== null) {
      if (!isObj(options)) throw new TypeError("options must be an object");
      preventClose = !!options.preventClose;
      preventAbort = !!options.preventAbort;
      preventCancel = !!options.preventCancel;
      signal = options.signal;
    }
    var reader = source.getReader();
    var writer;
    try { writer = dest.getWriter(); } catch (e) { reader.releaseLock(); throw e; }
    return new Promise(function (resolve, reject) {
      var shuttingDown = false;
      function finish(err, failed) {
        writer.releaseLock();
        reader.releaseLock();
        if (failed) reject(err); else resolve(undefined);
      }
      function shutdown(action, err, failed) {
        if (shuttingDown) return;
        shuttingDown = true;
        Promise.resolve().then(action).then(function () { finish(err, failed); }, function (e) { finish(e, true); });
      }
      function sourceFailed(e) {
        if (preventAbort) shutdown(noop, e, true);
        else shutdown(function () { return writer.abort(e); }, e, true);
      }
      function destFailed(e) {
        if (preventCancel) shutdown(noop, e, true);
        else shutdown(function () { return reader.cancel(e); }, e, true);
      }
      after(reader.closed, noop, sourceFailed);
      after(writer.closed, function () {
        destFailed(new TypeError("The destination closed before the pipe finished"));
      }, destFailed);
      if (signal) {
        var onAbort = function () {
          var e = signal.reason !== undefined ? signal.reason : new Error("Aborted");
          if (shuttingDown) return;
          shuttingDown = true;
          var acts = [];
          if (!preventAbort) acts.push(writer.abort(e));
          if (!preventCancel) acts.push(reader.cancel(e));
          Promise.all(acts).then(function () { finish(e, true); }, function (x) { finish(x, true); });
        };
        if (signal.aborted) { onAbort(); return; }
        signal.addEventListener("abort", onAbort);
      }
      function pump() {
        if (shuttingDown) return;
        after(writer.ready, function () {
          if (shuttingDown) return;
          after(reader.read(), function (res) {
            if (shuttingDown) return;
            if (res.done) {
              if (preventClose) shutdown(noop);
              else shutdown(function () { return writer.close(); });
              return;
            }
            handled(writer.write(res.value));
            pump();
          });
        });
      }
      pump();
    });
  }

  class ReadableStream {
    constructor(source, strategy) {
      if (source === undefined || source === null) source = {};
      if (!isObj(source)) throw new TypeError("underlyingSource must be an object");
      var type = source.type;
      var bytes = false;
      if (type !== undefined) {
        if (String(type) !== "bytes") throw new RangeError("Invalid underlyingSource type");
        bytes = true;
      }
      var r = {
        kind: "rs", state: "readable", err: undefined, reader: null, queue: [], qsize: 0,
        hwm: 1, sizeFn: null, started: false, pulling: false, pullAgain: false, closeRequested: false,
        disturbed: false, src: source, startFn: method(source, "start"), pullFn: method(source, "pull"),
        cancelFn: method(source, "cancel"), ctrl: null, bytes: bytes
      };
      if (bytes) {
        r.hwm = strategyHwm(strategy, 0);
        r.sizeFn = function (c) { return c.byteLength; };
      } else {
        r.sizeFn = strategySize(strategy);
        r.hwm = strategyHwm(strategy, 1);
      }
      this[S] = r;
      r.ctrl = new ReadableStreamDefaultController(INTERNAL, r);
      var started = r.startFn ? r.startFn.call(source, r.ctrl) : undefined;
      after(started, function () {
        r.started = true;
        rsPull(r);
      }, function (e) { rsError(r, e); });
    }
    get locked() { return brand(this, "rs").reader !== null; }
    cancel(reason) {
      var r = brandP(this, "rs");
      if (r instanceof Error) return rejected(r);
      if (r.reader) return rejected(new TypeError("ReadableStream is locked"));
      return rsCancel(r, reason);
    }
    getReader(options) {
      brand(this, "rs");
      if (isObj(options) && options.mode !== undefined) {
        if (String(options.mode) === "byob") throw new TypeError("BYOB readers are not supported");
        throw new TypeError("Invalid reader mode");
      }
      return new ReadableStreamDefaultReader(this);
    }
    pipeTo(dest, options) {
      try {
        var r = brand(this, "rs");
        brand(dest, "ws");
        if (r.reader) throw new TypeError("ReadableStream is locked");
        if (dest[S].writer) throw new TypeError("WritableStream is locked");
        return pipe(this, dest, options);
      } catch (e) { return rejected(e); }
    }
    pipeThrough(pair, options) {
      var r = brand(this, "rs");
      if (!isObj(pair)) throw new TypeError("transform must be an object");
      var readable = pair.readable, writable = pair.writable;
      brand(readable, "rs");
      brand(writable, "ws");
      if (r.reader) throw new TypeError("ReadableStream is locked");
      if (writable[S].writer) throw new TypeError("WritableStream is locked");
      handled(pipe(this, writable, options));
      return readable;
    }
    tee() {
      var r = brand(this, "rs");
      if (r.reader) throw new TypeError("ReadableStream is locked");
      return teeStream(this);
    }
    values(options) {
      brand(this, "rs");
      var reader = this.getReader();
      var preventCancel = isObj(options) && !!options.preventCancel;
      var it = {
        next: function () {
          return reader.read().then(function (res) {
            if (res.done) reader.releaseLock();
            return res;
          }, function (e) { reader.releaseLock(); throw e; });
        },
        "return": function (v) {
          var p = preventCancel ? Promise.resolve() : reader.cancel(v);
          reader.releaseLock();
          return p.then(function () { return { value: v, done: true }; });
        }
      };
      it[Symbol.asyncIterator] = function () { return this; };
      return it;
    }
    static from(src) { return readableFrom(src); }
  }
  ReadableStream.prototype[Symbol.asyncIterator] = ReadableStream.prototype.values;

  function wsUpdateReady(w) {
    var wr = w.writer;
    if (!wr || w.state !== "writable") return;
    var bp = w.closeRequested ? false : w.hwm - w.qsize <= 0;
    if (bp && wr.readySettled) { wr.ready = deferred(); wr.readySettled = false; }
    else if (!bp && !wr.readySettled) { wr.ready.resolve(undefined); wr.readySettled = true; }
  }
  function wsError(w, e) {
    if (w.state !== "writable") return;
    w.state = "errored";
    w.err = e;
    var q = w.ops;
    w.ops = [];
    w.qsize = 0;
    for (var i = 0; i < q.length; i++) q[i].d.reject(e);
    if (w.abortCtl) w.abortCtl.abort(e);
    var wr = w.writer;
    if (wr) {
      wr.closed.reject(e);
      handled(wr.closed.promise);
      if (wr.readySettled) wr.ready = deferred();
      wr.ready.reject(e);
      handled(wr.ready.promise);
      wr.readySettled = true;
    }
  }
  function wsAdvance(w) {
    if (!w.started || w.inflight || w.state !== "writable" || w.ops.length === 0) return;
    var op = w.inflight = w.ops.shift(), p;
    if (op.type === "write") {
      try { p = Promise.resolve(w.writeFn ? w.writeFn.call(w.sink, op.chunk, w.ctrl) : undefined); }
      catch (e) { p = Promise.reject(e); }
      after(p, function () {
        w.inflight = null;
        w.qsize -= op.size;
        op.d.resolve(undefined);
        wsUpdateReady(w);
        wsAdvance(w);
      }, function (e) {
        w.inflight = null;
        op.d.reject(e);
        wsError(w, e);
      });
    } else {
      try { p = Promise.resolve(w.closeFn ? w.closeFn.call(w.sink) : undefined); }
      catch (e) { p = Promise.reject(e); }
      after(p, function () {
        w.inflight = null;
        if (w.state === "writable") {
          w.state = "closed";
          if (w.writer) w.writer.closed.resolve(undefined);
        }
        op.d.resolve(undefined);
      }, function (e) {
        w.inflight = null;
        op.d.reject(e);
        wsError(w, e);
      });
    }
  }
  function wsWrite(w, chunk) {
    if (w.state === "errored") return rejected(w.err);
    if (w.state === "closed" || w.closeRequested) return rejected(new TypeError("Cannot write to a closing or closed stream"));
    var size;
    try { size = chunkSize(w.sizeFn, chunk); } catch (e) { wsError(w, e); return rejected(e); }
    var d = deferred();
    w.ops.push({ type: "write", chunk: chunk, size: size, d: d });
    w.qsize += size;
    wsUpdateReady(w);
    wsAdvance(w);
    return d.promise;
  }
  function wsClose(w) {
    if (w.state === "errored") return rejected(w.err);
    if (w.state === "closed" || w.closeRequested) return rejected(new TypeError("Stream is closed or closing"));
    w.closeRequested = true;
    var d = deferred();
    w.ops.push({ type: "close", d: d });
    wsUpdateReady(w);
    wsAdvance(w);
    return d.promise;
  }
  function wsAbort(w, reason) {
    if (w.state !== "writable") return Promise.resolve(undefined);
    var res;
    try { res = w.abortFn ? w.abortFn.call(w.sink, reason) : undefined; } catch (e) { res = Promise.reject(e); }
    wsError(w, reason);
    return Promise.resolve(res).then(function () { return undefined; });
  }

  class WritableStreamDefaultController {
    constructor(token, w) {
      if (token !== INTERNAL) throw new TypeError("Illegal constructor");
      this[S] = { kind: "wc", w: w };
    }
    get signal() {
      var w = brand(this, "wc").w;
      if (!w.abortCtl && typeof AbortController === "function") w.abortCtl = new AbortController();
      return w.abortCtl ? w.abortCtl.signal : undefined;
    }
    error(e) { wsError(brand(this, "wc").w, e); }
  }

  class WritableStreamDefaultWriter {
    constructor(stream) {
      var w = brand(stream, "ws");
      if (w.writer) throw new TypeError("WritableStream is locked");
      var wr = { kind: "wr", w: w, ready: deferred(), closed: deferred(), readySettled: false };
      this[S] = wr;
      w.writer = wr;
      handled(wr.closed.promise);
      handled(wr.ready.promise);
      if (w.state === "writable") wsUpdateReady(w);
      else if (w.state === "closed") { wr.ready.resolve(undefined); wr.readySettled = true; wr.closed.resolve(undefined); }
      else { wr.ready.reject(w.err); wr.readySettled = true; wr.closed.reject(w.err); }
      if (w.state === "writable" && w.hwm - w.qsize > 0 && !wr.readySettled) { wr.ready.resolve(undefined); wr.readySettled = true; }
    }
    get closed() {
      var wr = brandP(this, "wr");
      return wr instanceof Error ? rejected(wr) : wr.closed.promise;
    }
    get ready() {
      var wr = brandP(this, "wr");
      return wr instanceof Error ? rejected(wr) : wr.ready.promise;
    }
    get desiredSize() {
      var wr = brand(this, "wr");
      if (!wr.w) throw new TypeError("Writer has been released");
      var w = wr.w;
      if (w.state === "errored") return null;
      if (w.state === "closed") return 0;
      return w.hwm - w.qsize;
    }
    write(chunk) {
      var wr = brandP(this, "wr");
      if (wr instanceof Error) return rejected(wr);
      if (!wr.w) return rejected(new TypeError("Writer has been released"));
      return wsWrite(wr.w, chunk);
    }
    close() {
      var wr = brandP(this, "wr");
      if (wr instanceof Error) return rejected(wr);
      if (!wr.w) return rejected(new TypeError("Writer has been released"));
      return wsClose(wr.w);
    }
    abort(reason) {
      var wr = brandP(this, "wr");
      if (wr instanceof Error) return rejected(wr);
      if (!wr.w) return rejected(new TypeError("Writer has been released"));
      return wsAbort(wr.w, reason);
    }
    releaseLock() {
      var wr = brand(this, "wr");
      if (!wr.w) return;
      var err = new TypeError("Writer has been released");
      wr.ready = deferred();
      wr.ready.reject(err);
      handled(wr.ready.promise);
      if (wr.w.state === "writable") wr.closed.reject(err);
      else { wr.closed = deferred(); wr.closed.reject(err); }
      handled(wr.closed.promise);
      wr.w.writer = null;
      wr.w = null;
    }
  }

  class WritableStream {
    constructor(sink, strategy) {
      if (sink === undefined || sink === null) sink = {};
      if (!isObj(sink)) throw new TypeError("underlyingSink must be an object");
      if (sink.type !== undefined) throw new RangeError("Invalid underlyingSink type");
      var w = {
        kind: "ws", state: "writable", err: undefined, writer: null, ops: [], inflight: null, qsize: 0,
        started: false, closeRequested: false, sink: sink, writeFn: method(sink, "write"),
        closeFn: method(sink, "close"), abortFn: method(sink, "abort"), ctrl: null, abortCtl: null
      };
      var startFn = method(sink, "start");
      w.sizeFn = strategySize(strategy);
      w.hwm = strategyHwm(strategy, 1);
      this[S] = w;
      w.ctrl = new WritableStreamDefaultController(INTERNAL, w);
      var started = startFn ? startFn.call(sink, w.ctrl) : undefined;
      after(started, function () {
        w.started = true;
        wsAdvance(w);
      }, function (e) {
        w.started = true;
        wsError(w, e);
      });
    }
    get locked() { return brand(this, "ws").writer !== null; }
    abort(reason) {
      var w = brandP(this, "ws");
      if (w instanceof Error) return rejected(w);
      if (w.writer) return rejected(new TypeError("WritableStream is locked"));
      return wsAbort(w, reason);
    }
    close() {
      var w = brandP(this, "ws");
      if (w instanceof Error) return rejected(w);
      if (w.writer) return rejected(new TypeError("WritableStream is locked"));
      return wsClose(w);
    }
    getWriter() {
      brand(this, "ws");
      return new WritableStreamDefaultWriter(this);
    }
  }

  function tsSetBackpressure(t, b) {
    if (t.bpD) t.bpD.resolve(undefined);
    t.bpD = deferred();
    t.backpressure = b;
  }
  function tsErrorWritable(t, e) {
    wsError(t.ws, e);
    if (t.backpressure) tsSetBackpressure(t, false);
  }
  function tsError(t, e) {
    rsError(t.rs, e);
    tsErrorWritable(t, e);
  }
  function tsEnqueue(t, chunk) {
    var r = t.rs;
    if (r.closeRequested || r.state !== "readable") throw new TypeError("The readable side is not enqueueable");
    try { rsEnqueue(r, chunk); } catch (e) { tsErrorWritable(t, e); throw e; }
    var bp = !rsShouldPull(r);
    if (bp !== t.backpressure) tsSetBackpressure(t, true);
  }
  function tsTransform(t, chunk) {
    var tr = t.tr, res;
    if (!tr.transform) {
      try { tsEnqueue(t, chunk); } catch (e) { tsError(t, e); throw e; }
      return undefined;
    }
    try { res = tr.transform.call(tr, chunk, t.ctrl); } catch (e) { tsError(t, e); throw e; }
    return Promise.resolve(res).then(undefined, function (e) { tsError(t, e); throw e; });
  }

  class TransformStreamDefaultController {
    constructor(token, t) {
      if (token !== INTERNAL) throw new TypeError("Illegal constructor");
      this[S] = { kind: "tc", t: t };
    }
    get desiredSize() { return rsDesired(brand(this, "tc").t.rs); }
    enqueue(chunk) { tsEnqueue(brand(this, "tc").t, chunk); }
    error(e) { tsError(brand(this, "tc").t, e); }
    terminate() {
      var t = brand(this, "tc").t;
      if (!t.rs.closeRequested && t.rs.state === "readable") rsCloseRequest(t.rs);
      tsErrorWritable(t, new TypeError("The transform stream has been terminated"));
    }
  }

  class TransformStream {
    constructor(transformer, writableStrategy, readableStrategy) {
      if (transformer === undefined || transformer === null) transformer = {};
      if (!isObj(transformer)) throw new TypeError("transformer must be an object");
      if (transformer.readableType !== undefined || transformer.writableType !== undefined) throw new RangeError("Invalid type");
      method(transformer, "transform");
      method(transformer, "flush");
      var startFn = method(transformer, "start");
      var t = { kind: "ts", backpressure: false, bpD: null, ctrl: null, rs: null, ws: null, tr: transformer };
      var startD = deferred();
      var rstrat = { highWaterMark: strategyHwm(readableStrategy, 0), size: isObj(readableStrategy) ? readableStrategy.size : undefined };
      var readable = new ReadableStream({
        start: function () { return startD.promise; },
        pull: function () {
          tsSetBackpressure(t, false);
          return t.bpD.promise;
        },
        cancel: function (reason) {
          tsErrorWritable(t, reason);
          return Promise.resolve(undefined);
        }
      }, rstrat);
      var writable = new WritableStream({
        start: function () { return startD.promise; },
        write: function (chunk) {
          var go = function () {
            if (t.ws.state === "errored") throw t.ws.err;
            return tsTransform(t, chunk);
          };
          return t.backpressure ? t.bpD.promise.then(go) : go();
        },
        close: function () {
          var tr = t.tr, p;
          try { p = Promise.resolve(tr.flush ? tr.flush.call(tr, t.ctrl) : undefined); } catch (e) { p = Promise.reject(e); }
          return p.then(function () {
            if (t.rs.state === "errored") throw t.rs.err;
            if (t.rs.state === "readable" && !t.rs.closeRequested) rsCloseRequest(t.rs);
          }, function (e) { tsError(t, e); throw e; });
        },
        abort: function (reason) {
          rsError(t.rs, reason);
          return Promise.resolve(undefined);
        }
      }, writableStrategy);
      t.rs = readable[S];
      t.ws = writable[S];
      t.readable = readable;
      t.writable = writable;
      this[S] = t;
      tsSetBackpressure(t, true);
      t.ctrl = new TransformStreamDefaultController(INTERNAL, t);
      var started = startFn ? startFn.call(transformer, t.ctrl) : undefined;
      after(started, startD.resolve, startD.reject);
    }
    get readable() { return brand(this, "ts").readable; }
    get writable() { return brand(this, "ts").writable; }
  }

  function concat(parts, total) {
    var out = new Uint8Array(total), at = 0;
    for (var i = 0; i < parts.length; i++) { out.set(parts[i], at); at += parts[i].length; }
    return out;
  }
  function codecTransform(mode, format) {
    var chunks = [], total = 0;
    return new TransformStream({
      transform: function (chunk) {
        var u8 = toBytes(chunk);
        total += u8.length;
        if (total > MAX_BUFFERED) throw new RangeError("Input is too large");
        chunks.push(u8);
      },
      flush: function (c) {
        var all = concat(chunks, total);
        chunks = [];
        total = 0;
        var out = codec(mode, format, all);
        if (out.length > 0) c.enqueue(out);
      }
    });
  }
  function codecClass(name, mode) {
    var C = class {
      constructor(format) {
        format = String(format);
        if (format !== "gzip" && format !== "deflate" && format !== "deflate-raw") {
          throw new TypeError("Unsupported compression format: " + format);
        }
        this[S] = { kind: name, ts: codecTransform(mode, format) };
      }
      get readable() { return brand(this, name).ts.readable; }
      get writable() { return brand(this, name).ts.writable; }
    };
    Object.defineProperty(C, "name", { value: name });
    return C;
  }
  var CompressionStream = codecClass("CompressionStream", 0);
  var DecompressionStream = codecClass("DecompressionStream", 1);

  class TextEncoderStream {
    constructor() {
      var enc = new TextEncoder();
      this[S] = {
        kind: "tes",
        ts: new TransformStream({
          transform: function (chunk, c) {
            var u8 = enc.encode(String(chunk));
            if (u8.length) c.enqueue(u8);
          }
        })
      };
    }
    get encoding() { return "utf-8"; }
    get readable() { return brand(this, "tes").ts.readable; }
    get writable() { return brand(this, "tes").ts.writable; }
  }

  function utf8Complete(u8) {
    var n = u8.length, i = n - 1, back = 0;
    while (i >= 0 && back < 3 && (u8[i] & 0xC0) === 0x80) { i--; back++; }
    if (i < 0) return n;
    var b = u8[i], need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    return need > back + 1 ? i : n;
  }
  class TextDecoderStream {
    constructor(label, options) {
      var dec = new TextDecoder(label, options), pending = new Uint8Array(0);
      var l = label === undefined ? "utf-8" : String(label).trim().toLowerCase();
      var utf8 = l === "" || l === "utf-8" || l === "utf8" || l === "unicode-1-1-utf-8";
      this[S] = {
        kind: "tds",
        encoding: utf8 ? "utf-8" : l,
        ts: new TransformStream({
          transform: function (chunk, c) {
            var u8 = toBytes(chunk);
            var all = pending.length ? concat([pending, u8], pending.length + u8.length) : u8;
            var cut = utf8 ? utf8Complete(all) : all.length;
            pending = all.slice(cut);
            var text = dec.decode(all.subarray(0, cut));
            if (text.length) c.enqueue(text);
          },
          flush: function (c) {
            if (pending.length) {
              var text = dec.decode(pending);
              pending = new Uint8Array(0);
              if (text.length) c.enqueue(text);
            }
          }
        })
      };
    }
    get encoding() { return brand(this, "tds").encoding; }
    get readable() { return brand(this, "tds").ts.readable; }
    get writable() { return brand(this, "tds").ts.writable; }
  }

  function bytesStream(u8) {
    var at = 0;
    return new ReadableStream({
      pull: function (c) {
        if (at < u8.length) {
          var end = Math.min(at + 65536, u8.length);
          c.enqueue(u8.slice(at, end));
          at = end;
        }
        if (at >= u8.length) c.close();
      }
    });
  }

  function readAll(stream) {
    var d = deferred(), reader, chunks = [], total = 0;
    try { reader = stream.getReader(); } catch (e) { d.reject(e); return d.promise; }
    function step() {
      after(reader.read(), function (r) {
        try {
          if (r.done) { d.resolve(concat(chunks, total)); return; }
          var u8 = toBytes(r.value);
          total += u8.length;
          if (total > MAX_BUFFERED) throw new RangeError("Body is too large");
          chunks.push(u8);
        } catch (e) { reader.cancel(e).then(undefined, noop); d.reject(e); return; }
        step();
      }, d.reject);
    }
    step();
    return d.promise;
  }

  function consume(stream, kind, type) {
    return readAll(stream).then(function (u8) {
      if (kind === 0) return u8.buffer;
      if (kind === 2) return new Blob([u8], { type: type });
      var text = new TextDecoder().decode(u8);
      return kind === 3 ? JSON.parse(text) : text;
    });
  }

  return {
    bytesStream: bytesStream,
    consume: consume,
    ReadableStream: ReadableStream,
    ReadableStreamDefaultReader: ReadableStreamDefaultReader,
    ReadableStreamDefaultController: ReadableStreamDefaultController,
    WritableStream: WritableStream,
    WritableStreamDefaultWriter: WritableStreamDefaultWriter,
    WritableStreamDefaultController: WritableStreamDefaultController,
    TransformStream: TransformStream,
    TransformStreamDefaultController: TransformStreamDefaultController,
    CountQueuingStrategy: CountQueuingStrategy,
    ByteLengthQueuingStrategy: ByteLengthQueuingStrategy,
    CompressionStream: CompressionStream,
    DecompressionStream: DecompressionStream,
    TextEncoderStream: TextEncoderStream,
    TextDecoderStream: TextDecoderStream
  };
})
)
