function fuzz(name, n) {
  var s = 12345, base = W[name];
  for (var i = 0; i < n; i++) {
    var m = new Uint8Array(base.length), k;
    m.set(base);
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    k = 1 + (s % 3);
    for (var j = 0; j < k; j++) {
      s = (s * 1103515245 + 12345) & 0x7fffffff;
      m[8 + (s % (m.length - 8))] = (s >> 8) & 255;
    }
    try { WebAssembly.validate(m); } catch (e) {}
  }
}
["ctl", "mem", "tbl", "imp", "glob", "ext", "exports"].forEach(function (n) { fuzz(n, 600); });
var order = [];
function Resp(buf) { this.arrayBuffer = function () { return Promise.resolve(buf); }; }
function resp(buf) { return typeof Response === "function" ? new Response(buf, { headers: { "Content-Type": "application/wasm" } }) : new Resp(buf); }
var pending = [
  WebAssembly.instantiate(W.ctl).then(function (r) { order.push("inst:" + Object.keys(r).join("/") + ":" + r.instance.exports.sum(4) + ":" + (r.module instanceof WebAssembly.Module)); }),
  WebAssembly.instantiate(new WebAssembly.Module(W.exports)).then(function (r) { order.push("inst2:" + (r instanceof WebAssembly.Instance)); }),
  WebAssembly.compile(W.ctl).then(function (m) { order.push("compile:" + (m instanceof WebAssembly.Module)); }),
  WebAssembly.compile(new Uint8Array(3)).then(null, function (e) { order.push("compile-bad:" + e.name); }),
  WebAssembly.instantiate(W.imp, {}).then(null, function (e) { order.push("link:" + e.name); }),
  WebAssembly.instantiate(5).then(null, function (e) { order.push("type:" + e.name); }),
  WebAssembly.instantiateStreaming(resp(W.ctl.buffer), {}).then(function (r) { order.push("stream:" + r.instance.exports.sum(5)); }),
  WebAssembly.compileStreaming(Promise.resolve(resp(W.ctl.buffer))).then(function (m) { order.push("cstream:" + (m instanceof WebAssembly.Module)); })
];
Promise.all(pending).then(function () {
  var got = order.slice().sort().join("|");
  if (RECORDING) E["promises"] = got; else if (E["promises"] !== got) failures.push("promises: got " + got + " want " + E["promises"]);
  __result = failures.join("\n");
});
