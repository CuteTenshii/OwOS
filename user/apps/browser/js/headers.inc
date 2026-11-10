/* Headers, shared by the page (fetchapi.c) and dedicated workers (webworker.c); same conventions as
 * workerscope_prelude.inc (no single quotes, backslashes or // comments; every statement ends with a semicolon).
 * Called as f(globalThis): defines globalThis.Headers and returns it. */
JS(
(function (g) {
  var def = Object.defineProperty;
  function hide(o, k, v) { def(o, k, { value: v, writable: true, configurable: true, enumerable: false }); }
  function method(proto, name, fn) { hide(proto, name, fn); }
  function typeError(msg) { return new TypeError(msg); }
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
  return Headers;
})
)
