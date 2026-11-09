// Regenerates tests/wasm_vectors.h for tests/wasm_test.c. Needs node and wabt (wat2wasm).
//   node tests/wasm/gen.js
// The expected values come from running the same script on V8, so the suite compares OwOS against it. The numeric
// and SIMD operator modules are built here, and only a hash of each operator's results is stored.
const { execSync } = require("child_process");
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const dir = __dirname;

const FEATURES = "--enable-threads";
function wasmOf(file) {
  const out = path.join(require("os").tmpdir(), "owos_" + path.basename(file, ".wat") + ".wasm");
  execSync(`wat2wasm ${FEATURES} "${file}" -o "${out}"`);
  return fs.readFileSync(out);
}

const hexf = (v) => { const b = Buffer.alloc(8); b.writeDoubleBE(v); return b.toString("hex"); };
const F = [0, -0, 1, -1, 0.5, -0.5, 1.5, 2.5, -2.5, 3.7, NaN, Infinity, -Infinity, 1e10, -1e10, 2147483647.9, -2147483648.9,
  2147483648, 4294967295.5, 4294967296, 9.3e18, -9.3e18, 1.8446744073709552e19, 1e-45, 3.4028235e38, 1e39, -2147483904,
  16777217, 9223372036854775807, -9223372036854775808];
const FHEX = F.map(hexf);
const I32 = ["0", "1", "-1", "2", "31", "32", "33", "2147483647", "-2147483648", "305419896", "-12345", "255", "256", "65535", "65536", "-2147450880"];
const I64 = ["0", "1", "-1", "2", "63", "64", "65", "9223372036854775807", "-9223372036854775808", "1311768467463790320", "-12345", "4294967295", "4294967296", "-9223372034707292160", "255"];

// ---- scalar operators --------------------------------------------------------------------------------------------
const nops = [];
const add = (name, args, res) => nops.push({ name, args, res });
for (const T of ["i32", "i64"]) {
  for (const o of ["add", "sub", "mul", "div_s", "div_u", "rem_s", "rem_u", "and", "or", "xor", "shl", "shr_s", "shr_u", "rotl", "rotr"]) add(`${T}.${o}`, [T, T], T);
  for (const o of ["eq", "ne", "lt_s", "lt_u", "gt_s", "gt_u", "le_s", "le_u", "ge_s", "ge_u"]) add(`${T}.${o}`, [T, T], "i32");
  for (const o of ["clz", "ctz", "popcnt"]) add(`${T}.${o}`, [T], T);
  add(`${T}.eqz`, [T], "i32");
}
for (const o of ["extend8_s", "extend16_s"]) { add(`i32.${o}`, ["i32"], "i32"); add(`i64.${o}`, ["i64"], "i64"); }
add("i64.extend32_s", ["i64"], "i64");
for (const T of ["f32", "f64"]) {
  for (const o of ["add", "sub", "mul", "div", "min", "max", "copysign"]) add(`${T}.${o}`, [T, T], T);
  for (const o of ["eq", "ne", "lt", "gt", "le", "ge"]) add(`${T}.${o}`, [T, T], "i32");
  for (const o of ["abs", "neg", "ceil", "floor", "trunc", "nearest", "sqrt"]) add(`${T}.${o}`, [T], T);
}
for (const [n, a, r] of [["i32.wrap_i64", "i64", "i32"], ["i32.trunc_f32_s", "f32", "i32"], ["i32.trunc_f32_u", "f32", "i32"], ["i32.trunc_f64_s", "f64", "i32"], ["i32.trunc_f64_u", "f64", "i32"],
  ["i64.extend_i32_s", "i32", "i64"], ["i64.extend_i32_u", "i32", "i64"], ["i64.trunc_f32_s", "f32", "i64"], ["i64.trunc_f32_u", "f32", "i64"], ["i64.trunc_f64_s", "f64", "i64"], ["i64.trunc_f64_u", "f64", "i64"],
  ["f32.convert_i32_s", "i32", "f32"], ["f32.convert_i32_u", "i32", "f32"], ["f32.convert_i64_s", "i64", "f32"], ["f32.convert_i64_u", "i64", "f32"], ["f32.demote_f64", "f64", "f32"],
  ["f64.convert_i32_s", "i32", "f64"], ["f64.convert_i32_u", "i32", "f64"], ["f64.convert_i64_s", "i64", "f64"], ["f64.convert_i64_u", "i64", "f64"], ["f64.promote_f32", "f32", "f64"],
  ["i32.reinterpret_f32", "f32", "i32"], ["i64.reinterpret_f64", "f64", "i64"], ["f32.reinterpret_i32", "i32", "f32"], ["f64.reinterpret_i64", "i64", "f64"],
  ["i32.trunc_sat_f32_s", "f32", "i32"], ["i32.trunc_sat_f32_u", "f32", "i32"], ["i32.trunc_sat_f64_s", "f64", "i32"], ["i32.trunc_sat_f64_u", "f64", "i32"],
  ["i64.trunc_sat_f32_s", "f32", "i64"], ["i64.trunc_sat_f32_u", "f32", "i64"], ["i64.trunc_sat_f64_s", "f64", "i64"], ["i64.trunc_sat_f64_u", "f64", "i64"]]) add(n, [a], r);

// ---- SIMD operators ----------------------------------------------------------------------------------------------
const sops = [];
const A = "(v128.load (i32.const 0))", B = "(v128.load offset=16 (i32.const 0))", C = "(v128.load offset=32 (i32.const 0))";
const store = (e) => `(v128.store offset=64 (i32.const 0) ${e})`;
const un = (op) => sops.push({ n: op, body: store(`(${op} ${A})`) });
const bin = (op) => sops.push({ n: op, body: store(`(${op} ${A} ${B})`) });
const test = (op) => sops.push({ n: op, ret: "i32", body: `(${op} ${A})` });
const shift = (op) => sops.push({ n: op, body: store(`(${op} ${A} (local.get 0))`) });
const cmp = ["eq", "ne", "lt_s", "lt_u", "gt_s", "gt_u", "le_s", "le_u", "ge_s", "ge_u"];
const words = (s) => s.split(/\s+/).filter(Boolean);
words(`i8x16.abs i8x16.neg i8x16.popcnt i16x8.abs i16x8.neg i16x8.extend_low_i8x16_s i16x8.extend_high_i8x16_s i16x8.extend_low_i8x16_u i16x8.extend_high_i8x16_u i16x8.extadd_pairwise_i8x16_s i16x8.extadd_pairwise_i8x16_u
i32x4.abs i32x4.neg i32x4.extend_low_i16x8_s i32x4.extend_high_i16x8_s i32x4.extend_low_i16x8_u i32x4.extend_high_i16x8_u i32x4.extadd_pairwise_i16x8_s i32x4.extadd_pairwise_i16x8_u i32x4.trunc_sat_f32x4_s i32x4.trunc_sat_f32x4_u i32x4.trunc_sat_f64x2_s_zero i32x4.trunc_sat_f64x2_u_zero
i64x2.abs i64x2.neg i64x2.extend_low_i32x4_s i64x2.extend_high_i32x4_s i64x2.extend_low_i32x4_u i64x2.extend_high_i32x4_u
f32x4.abs f32x4.neg f32x4.sqrt f32x4.ceil f32x4.floor f32x4.trunc f32x4.nearest f32x4.convert_i32x4_s f32x4.convert_i32x4_u f32x4.demote_f64x2_zero
f64x2.abs f64x2.neg f64x2.sqrt f64x2.ceil f64x2.floor f64x2.trunc f64x2.nearest f64x2.promote_low_f32x4 f64x2.convert_low_i32x4_s f64x2.convert_low_i32x4_u v128.not`).forEach(un);
const binl = [];
for (const s of ["i8x16", "i16x8", "i32x4"]) for (const c of cmp) binl.push(`${s}.${c}`);
binl.push("i64x2.eq", "i64x2.ne", "i64x2.lt_s", "i64x2.gt_s", "i64x2.le_s", "i64x2.ge_s");
for (const s of ["f32x4", "f64x2"]) for (const c of ["eq", "ne", "lt", "gt", "le", "ge"]) binl.push(`${s}.${c}`);
binl.push(...words(`i8x16.add i8x16.add_sat_s i8x16.add_sat_u i8x16.sub i8x16.sub_sat_s i8x16.sub_sat_u i8x16.min_s i8x16.min_u i8x16.max_s i8x16.max_u i8x16.avgr_u i8x16.narrow_i16x8_s i8x16.narrow_i16x8_u i8x16.swizzle
i16x8.add i16x8.add_sat_s i16x8.add_sat_u i16x8.sub i16x8.sub_sat_s i16x8.sub_sat_u i16x8.mul i16x8.min_s i16x8.min_u i16x8.max_s i16x8.max_u i16x8.avgr_u i16x8.q15mulr_sat_s i16x8.narrow_i32x4_s i16x8.narrow_i32x4_u i16x8.extmul_low_i8x16_s i16x8.extmul_high_i8x16_s i16x8.extmul_low_i8x16_u i16x8.extmul_high_i8x16_u
i32x4.add i32x4.sub i32x4.mul i32x4.min_s i32x4.min_u i32x4.max_s i32x4.max_u i32x4.dot_i16x8_s i32x4.extmul_low_i16x8_s i32x4.extmul_high_i16x8_s i32x4.extmul_low_i16x8_u i32x4.extmul_high_i16x8_u
i64x2.add i64x2.sub i64x2.mul i64x2.extmul_low_i32x4_s i64x2.extmul_high_i32x4_s i64x2.extmul_low_i32x4_u i64x2.extmul_high_i32x4_u
f32x4.add f32x4.sub f32x4.mul f32x4.div f32x4.min f32x4.max f32x4.pmin f32x4.pmax f64x2.add f64x2.sub f64x2.mul f64x2.div f64x2.min f64x2.max f64x2.pmin f64x2.pmax v128.and v128.andnot v128.or v128.xor`));
binl.forEach(bin);
["v128.any_true", "i8x16.all_true", "i8x16.bitmask", "i16x8.all_true", "i16x8.bitmask", "i32x4.all_true", "i32x4.bitmask", "i64x2.all_true", "i64x2.bitmask"].forEach(test);
for (const s of ["i8x16", "i16x8", "i32x4", "i64x2"]) for (const o of ["shl", "shr_s", "shr_u"]) shift(`${s}.${o}`);
sops.push({ n: "v128.bitselect", body: store(`(v128.bitselect ${A} ${B} ${C})`) });
const lanes = { i8x16: 16, i16x8: 8, i32x4: 4, i64x2: 2, f32x4: 4, f64x2: 2 };
const sc = { i8x16: "i32", i16x8: "i32", i32x4: "i32", i64x2: "i64", f32x4: "f32", f64x2: "f64" };
for (const s in lanes) {
  sops.push({ n: `${s}.splat`, body: store(`(${s}.splat (${sc[s]}.load (i32.const 0)))`) });
  for (let l = 0; l < lanes[s]; l++) {
    for (const e of s === "i8x16" || s === "i16x8" ? ["extract_lane_s", "extract_lane_u"] : ["extract_lane"])
      sops.push({ n: `${s}.${e}@${l}`, ret: sc[s], body: `(${s}.${e} ${l} ${A})` });
    sops.push({ n: `${s}.replace_lane@${l}`, body: store(`(${s}.replace_lane ${l} ${A} (${sc[s]}.load offset=16 (i32.const 0)))`) });
  }
}
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
for (let t = 0; t < 12; t++) {
  const l = [];
  for (let i = 0; i < 16; i++) l.push(t < 2 ? (t ? i + 16 : i) : rnd() % 32);
  sops.push({ n: `shuffle@${l.join(",")}`, body: store(`(i8x16.shuffle ${l.join(" ")} ${A} ${B})`) });
}
sops.push({ n: "v128.const", body: store("(v128.const i32x4 0x01020304 0x80000000 -1 0x7fffffff)") });
for (const o of ["load", "load8x8_s", "load8x8_u", "load16x4_s", "load16x4_u", "load32x2_s", "load32x2_u", "load8_splat", "load16_splat", "load32_splat", "load64_splat", "load32_zero", "load64_zero"])
  sops.push({ n: "v128." + o, body: store(`(v128.${o} offset=3 (i32.const 1))`) });
for (const [o, n] of [["load8_lane", 16], ["load16_lane", 8], ["load32_lane", 4], ["load64_lane", 2]])
  for (let l = 0; l < n; l += Math.max(1, n >> 2)) sops.push({ n: `v128.${o}@${l}`, body: store(`(v128.${o} offset=5 ${l} (i32.const 1) ${A})`) });
for (const [o, n] of [["store8_lane", 16], ["store16_lane", 8], ["store32_lane", 4], ["store64_lane", 2]])
  for (let l = 0; l < n; l += Math.max(1, n >> 2)) sops.push({ n: `v128.${o}@${l}`, body: `${store(A)} (v128.${o} offset=70 ${l} (i32.const 0) ${B})` });

const tmp = require("os").tmpdir();
function build(name, wat) {
  const f = path.join(tmp, "owos_" + name + ".wat");
  fs.writeFileSync(f, wat);
  return wasmOf(f);
}
const numWat = "(module\n" + nops.map((o, i) => `(func (export "f${i}") ${o.args.map((a) => `(param ${a})`).join(" ")} (result ${o.res}) ${o.args.map((a, k) => `local.get ${k}`).join(" ")} ${o.name})`).join("\n") + ")\n";
const simdWat = '(module (memory (export "m") 1)\n' + sops.map((o, i) => `(func (export "f${i}") (param i32) ${o.ret ? `(result ${o.ret})` : ""} ${o.body})`).join("\n") + ")\n";

const mods = {};
for (const f of fs.readdirSync(dir).filter((f) => f.endsWith(".wat"))) mods[f.slice(0, -4)] = wasmOf(path.join(dir, f));
mods.num = build("num", numWat);
mods.simd = build("simd", simdWat);

const F32S = [0, -0, 1, -1, 0.5, -0.5, 1.5, 2.5, -2.5, 3.7, NaN, Infinity, -Infinity, 1e10, -1e10, 2147483647.9, 2147483648, 4294967296, 1e-40, 3e38, -3e38, 0.1, 100.5, -7.25].map(hexf);
const F64S = [0, -0, 1, -1, 0.5, -0.5, 1.5, 2.5, -2.5, 3.7, NaN, Infinity, -Infinity, 1e10, -1e10, 2147483647.9, -2147483648.9, 4294967295.5, 4294967296, 9.3e18, -9.3e18, 1.8e19, 1e300, 0.1].map(hexf);

const runners = `
function dbl(h) { var b = new BigUint64Array(1), d = new Float64Array(b.buffer); b[0] = BigInt("0x" + h); return d[0]; }
function big(s) { return s[0] === "-" ? -BigInt(s.slice(1)) : BigInt(s); }
var NUM_OPS = ${JSON.stringify(nops.map((o) => [o.name, o.args]))};
var NUM_IN = { i32: ${JSON.stringify(I32)}, i64: ${JSON.stringify(I64)}, f32: ${JSON.stringify(FHEX)}, f64: ${JSON.stringify(FHEX)} };
function numArg(t, s) { return t === "i32" ? Number(s) | 0 : t === "i64" ? big(s) : dbl(s); }
function runNum() {
  var X = new WebAssembly.Instance(new WebAssembly.Module(W.num), {}).exports;
  for (var i = 0; i < NUM_OPS.length; i++) {
    var out = [], a = NUM_OPS[i][1], f = X["f" + i];
    var go = function (args) {
      try { out.push(fmt(args.length === 1 ? f(args[0]) : f(args[0], args[1]))); } catch (e) { out.push("trap:" + e.name); }
    };
    if (a.length === 1) NUM_IN[a[0]].forEach(function (x) { go([numArg(a[0], x)]); });
    else NUM_IN[a[0]].forEach(function (x) { NUM_IN[a[1]].forEach(function (y) { go([numArg(a[0], x), numArg(a[1], y)]); }); });
    chk("num:" + NUM_OPS[i][0], out.join(","));
  }
}
var SIMD_OPS = ${JSON.stringify(sops.map((o) => [o.n, o.ret || ""]))};
var F32S = ${JSON.stringify(F32S)}, F64S = ${JSON.stringify(F64S)};
function runSimd() {
  var X = new WebAssembly.Instance(new WebAssembly.Module(W.simd), {}).exports, M = new Uint8Array(X.m.buffer);
  var s = 7;
  function rnd() { s = (s * 1103515245 + 12345) & 0x7fffffff; return s; }
  function toArr(u) { var r = []; for (var i = 0; i < u.length; i++) r.push(u[i]); return r; }
  function mk(f) { var v = []; for (var i = 0; i < 16; i++) v.push(f(i)); return v; }
  function fvec32() { var a = new Float32Array(4); for (var i = 0; i < 4; i++) a[i] = dbl(F32S[rnd() % F32S.length]); return toArr(new Uint8Array(a.buffer)); }
  function fvec64() { var a = new Float64Array(2); for (var i = 0; i < 2; i++) a[i] = dbl(F64S[rnd() % F64S.length]); return toArr(new Uint8Array(a.buffer)); }
  var V = [mk(function () { return 0; }), mk(function () { return 255; }), mk(function () { return 128; }), mk(function () { return 127; }), mk(function (i) { return i; }), mk(function (i) { return (i * 17) & 255; })];
  for (var i = 0; i < 4; i++) V.push(mk(function () { return rnd() & 255; }));
  for (var i = 0; i < 4; i++) V.push(fvec32());
  for (var i = 0; i < 3; i++) V.push(fvec64());
  function canon(bytes, mode) {
    var u = new Uint8Array(bytes), w = new Uint32Array(u.buffer), i;
    if (mode === "f32") { var f = new Float32Array(u.buffer); for (i = 0; i < 4; i++) if (f[i] !== f[i]) w[i] = 0x7fc00000; }
    if (mode === "f64") { var g = new Float64Array(u.buffer); for (i = 0; i < 2; i++) if (g[i] !== g[i]) { w[2 * i] = 0; w[2 * i + 1] = 0x7ff80000; } }
    return toArr(u);
  }
  function hex(b) { return b.map(function (x) { return (x < 16 ? "0" : "") + x.toString(16); }).join(""); }
  var tops = [0, 1, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65];
  for (var i = 0; i < SIMD_OPS.length; i++) {
    var f = X["f" + i], name = SIMD_OPS[i][0], ret = SIMD_OPS[i][1], out = [], mode = "";
    if (/^f32x4\\.(?!extract|eq|ne|lt|gt|le|ge)/.test(name)) mode = "f32";
    if (/^f64x2\\.(?!extract|eq|ne|lt|gt|le|ge)/.test(name)) mode = "f64";
    if (/^f32x4\\.convert_i/.test(name)) mode = "f32";
    if (/^f64x2\\.(convert|promote)/.test(name)) mode = "f64";
    var shifts = /x.*\\.(shl|shr)/.test(name) ? tops : [0];
    var nargs = /^v128\\.(any|bitselect)|swizzle|all_true|bitmask|\\.(abs|neg|not|popcnt|sqrt|ceil|floor|trunc|nearest|extend|extadd|trunc_sat|convert|demote|promote)/.test(name) ? 1 : 2;
    var pairs = [];
    for (var a = 0; a < V.length; a++) for (var b = 0; b < (nargs === 2 ? V.length : 1); b++) pairs.push([a, b]);
    if (/bitselect/.test(name)) pairs = pairs.filter(function (p, k) { return k % 5 === 0; });
    for (var pi = 0; pi < pairs.length; pi++) {
      for (var si = 0; si < shifts.length; si++) {
        if (si > 0 && pi > 6) break;
        M.set(V[pairs[pi][0]], 0);
        M.set(V[pairs[pi][1]], 16);
        M.set(V[(pairs[pi][0] + 3) % V.length], 32);
        M.fill(0, 64, 96);
        try {
          var r = f(shifts[si]);
          out.push(ret ? fmt(r) : hex(canon(toArr(M.subarray(64, 96)), mode)));
        } catch (e) { out.push("trap:" + e.name); }
      }
    }
    chk("simd:" + name, out.join(","));
  }
}
`;

const hexOf = (b) => b.toString("hex");
const apiBody = fs.readFileSync(path.join(dir, "api.js"), "utf8");
function script(E, H, recording) {
  let s = "var RECORDING = " + recording + ";\nvar E = " + JSON.stringify(E) + ";\nvar H = " + JSON.stringify(H) + ";\nvar W = {};\n";
  s += "function hex(s) { var u = new Uint8Array(s.length / 2); for (var i = 0; i < u.length; i++) u[i] = parseInt(s.substr(i * 2, 2), 16); return u; }\n";
  for (const k of Object.keys(mods)) s += `W[${JSON.stringify(k)}] = hex(${JSON.stringify(hexOf(mods[k]))});\n`;
  s += "function fnv(s) { var h = 0x811c9dc5; for (var i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193); } return (h >>> 0).toString(16); }\n";
  s += "function chk(label, str) { var h = fnv(str); if (RECORDING) H[label] = h; else if (H[label] !== h) failures.push(label + \": results differ\"); }\n";
  s += apiBody;
  s += runners;
  s += "runNum();\nrunSimd();\n";
  s += fs.readFileSync(path.join(dir, "tail.js"), "utf8");
  return s;
}

// Record on V8
global.gc = () => {};
const ctx = { SharedArrayBuffer, structuredClone, Response, gc() {}, console, Math, WebAssembly, Uint8Array, Float32Array, Float64Array, BigUint64Array, BigInt, Number, String, Object, Array, Promise, parseInt, Error, TypeError, RangeError, DataView, Uint32Array };
vm.createContext(ctx);
vm.runInContext(script({}, {}, true) + "\nvar __E = E, __H = H;", ctx);
// Promise reactions are asynchronous on V8: wait for them before reading the recorded table
setTimeout(() => {
  const E = ctx.__E, H = ctx.__H;
  const final = script(E, H, false);
  // Verify the generated script passes on V8 itself
  const ctx2 = { SharedArrayBuffer, structuredClone, Response, gc() {}, console, Math, WebAssembly, Uint8Array, Float32Array, Float64Array, BigUint64Array, BigInt, Number, String, Object, Array, Promise, parseInt, Error, TypeError, RangeError, DataView, Uint32Array };
  vm.createContext(ctx2);
  vm.runInContext(final, ctx2);
  setTimeout(() => {
    if (ctx2.__result !== "") { console.error("V8 disagrees with its own recording:\n" + ctx2.__result); process.exit(1); }
    const lines = final.match(/[^\n]*\n|[^\n]+$/g);
    let out = "/* wasm_vectors.h: generated by tests/wasm/gen.js, do not edit. */\n#ifndef OWOS_WASM_VECTORS_H\n#define OWOS_WASM_VECTORS_H\n\nstatic const char *const k_wasm_script[] = {\n";
    for (const l of lines) out += "    " + JSON.stringify(l).replace(/\?/g, "\\?") + ",\n";
    out += "    0};\n\n/* tests/wasm/counter.wat, for the threads test */\nstatic const unsigned char k_wasm_counter[] = {" + [...mods.counter].join(",") + "};\n\n#endif\n";
    fs.writeFileSync(path.join(dir, "..", "wasm_vectors.h"), out);
    console.log("wrote tests/wasm_vectors.h (" + out.length + " bytes, " + Object.keys(E).length + " api checks, " + Object.keys(H).length + " operator hashes)");
  }, 200);
}, 200);
