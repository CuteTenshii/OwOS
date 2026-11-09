/* The script half of the Streams API, stringified by the preprocessor (see streams.c): the strategies, tee, async
 * iteration, ReadableStream.from, the text and compression streams and the body helpers, all on top of the native
 * classes in streamscore.c (passed in as N). Keep it free of single quotes and backslashes, and end every statement
 * with a semicolon: the line breaks are lost. This file is not run through clang-format, which would split operators
 * such as === apart. */
JS(
(function (codec, N) {
  var S = Symbol("stream");
  var MAX_BUFFERED = 67108864;
  var ReadableStream = N.ReadableStream;
  var TransformStream = N.TransformStream;
  var after = N.after;
  var lockedGet = Object.getOwnPropertyDescriptor(ReadableStream.prototype, "locked").get;
  function noop() {}
  function isObj(v) { return (typeof v === "object" && v !== null) || typeof v === "function"; }
  function deferred() {
    var d = {};
    d.promise = new Promise(function (res, rej) { d.resolve = res; d.reject = rej; });
    return d;
  }
  function handled(p) { p.then(undefined, noop); return p; }
  function rejected(e) { return handled(Promise.reject(e)); }
  function brand(o, kind) {
    var s = o !== null && o !== undefined ? o[S] : undefined;
    if (!s || s.kind !== kind) throw new TypeError("Illegal invocation");
    return s;
  }
  function toHwm(v) {
    var n = Number(v);
    if (n !== n || n < 0) throw new RangeError("Invalid highWaterMark");
    return n;
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


  function defineMethod(obj, name, fn) {
    Object.defineProperty(obj, name, { value: fn, writable: true, enumerable: false, configurable: true });
  }
  defineMethod(ReadableStream.prototype, "tee", function () {
    if (lockedGet.call(this)) throw new TypeError("ReadableStream is locked");
    return teeStream(this);
  });
  defineMethod(ReadableStream.prototype, "values", function (options) {
    lockedGet.call(this);
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
  });
  defineMethod(ReadableStream, "from", function (src) { return readableFrom(src); });
  ReadableStream.prototype[Symbol.asyncIterator] = ReadableStream.prototype.values;

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

  /* A stream over bytes already in memory: one chunk, so reading it costs the same whatever its size */
  function bytesStream(u8) {
    var sent = false;
    return new ReadableStream({
      pull: function (c) {
        if (!sent) {
          sent = true;
          if (u8.length) c.enqueue(u8);
        }
        c.close();
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
    ReadableStreamDefaultReader: N.ReadableStreamDefaultReader,
    ReadableStreamDefaultController: N.ReadableStreamDefaultController,
    WritableStream: N.WritableStream,
    WritableStreamDefaultWriter: N.WritableStreamDefaultWriter,
    WritableStreamDefaultController: N.WritableStreamDefaultController,
    TransformStream: TransformStream,
    TransformStreamDefaultController: N.TransformStreamDefaultController,
    CountQueuingStrategy: CountQueuingStrategy,
    ByteLengthQueuingStrategy: ByteLengthQueuingStrategy,
    CompressionStream: CompressionStream,
    DecompressionStream: DecompressionStream,
    TextEncoderStream: TextEncoderStream,
    TextDecoderStream: TextDecoderStream
  };
})
)
