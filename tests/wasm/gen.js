// Regenerates tests/wasm/data.js for tests/wasm_test.c. Needs node and wabt (wat2wasm).
//   node tests/wasm/gen.js
// The expected values come from running the same script on V8, so the suite compares OwOS against it. The numeric
// and SIMD operator modules are built here, and only a hash of each operator's results is stored.
const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const dir = __dirname;

const FEATURES = '--enable-threads';
function wasmOf(file) {
  const out = path.join(require('os').tmpdir(), 'owos_' + path.basename(file, '.wat') + '.wasm');
  execSync(`wat2wasm ${FEATURES} "${file}" -o "${out}"`);
  return fs.readFileSync(out);
}

const hexf = (v) => {
  const b = Buffer.alloc(8);
  b.writeDoubleBE(v);
  return b.toString('hex');
};
const F = [
  0,
  -0,
  1,
  -1,
  0.5,
  -0.5,
  1.5,
  2.5,
  -2.5,
  3.7,
  NaN,
  Infinity,
  -Infinity,
  1e10,
  -1e10,
  2147483647.9,
  -2147483648.9,
  2147483648,
  4294967295.5,
  4294967296,
  9.3e18,
  -9.3e18,
  1.8446744073709552e19,
  1e-45,
  3.4028235e38,
  1e39,
  -2147483904,
  16777217,
  9223372036854775808,
  -9223372036854775808,
];
const FHEX = F.map(hexf);
const I32 = [
  '0',
  '1',
  '-1',
  '2',
  '31',
  '32',
  '33',
  '2147483647',
  '-2147483648',
  '305419896',
  '-12345',
  '255',
  '256',
  '65535',
  '65536',
  '-2147450880',
];
const I64 = [
  '0',
  '1',
  '-1',
  '2',
  '63',
  '64',
  '65',
  '9223372036854775807',
  '-9223372036854775808',
  '1311768467463790320',
  '-12345',
  '4294967295',
  '4294967296',
  '-9223372034707292160',
  '255',
];

// ---- scalar operators --------------------------------------------------------------------------------------------
const nops = [];
const add = (name, args, res) => nops.push({ name, args, res });
for (const T of ['i32', 'i64']) {
  for (const o of [
    'add',
    'sub',
    'mul',
    'div_s',
    'div_u',
    'rem_s',
    'rem_u',
    'and',
    'or',
    'xor',
    'shl',
    'shr_s',
    'shr_u',
    'rotl',
    'rotr',
  ])
    add(`${T}.${o}`, [T, T], T);
  for (const o of ['eq', 'ne', 'lt_s', 'lt_u', 'gt_s', 'gt_u', 'le_s', 'le_u', 'ge_s', 'ge_u'])
    add(`${T}.${o}`, [T, T], 'i32');
  for (const o of ['clz', 'ctz', 'popcnt']) add(`${T}.${o}`, [T], T);
  add(`${T}.eqz`, [T], 'i32');
}
for (const o of ['extend8_s', 'extend16_s']) {
  add(`i32.${o}`, ['i32'], 'i32');
  add(`i64.${o}`, ['i64'], 'i64');
}
add('i64.extend32_s', ['i64'], 'i64');
for (const T of ['f32', 'f64']) {
  for (const o of ['add', 'sub', 'mul', 'div', 'min', 'max', 'copysign']) add(`${T}.${o}`, [T, T], T);
  for (const o of ['eq', 'ne', 'lt', 'gt', 'le', 'ge']) add(`${T}.${o}`, [T, T], 'i32');
  for (const o of ['abs', 'neg', 'ceil', 'floor', 'trunc', 'nearest', 'sqrt']) add(`${T}.${o}`, [T], T);
}
for (const [n, a, r] of [
  ['i32.wrap_i64', 'i64', 'i32'],
  ['i32.trunc_f32_s', 'f32', 'i32'],
  ['i32.trunc_f32_u', 'f32', 'i32'],
  ['i32.trunc_f64_s', 'f64', 'i32'],
  ['i32.trunc_f64_u', 'f64', 'i32'],
  ['i64.extend_i32_s', 'i32', 'i64'],
  ['i64.extend_i32_u', 'i32', 'i64'],
  ['i64.trunc_f32_s', 'f32', 'i64'],
  ['i64.trunc_f32_u', 'f32', 'i64'],
  ['i64.trunc_f64_s', 'f64', 'i64'],
  ['i64.trunc_f64_u', 'f64', 'i64'],
  ['f32.convert_i32_s', 'i32', 'f32'],
  ['f32.convert_i32_u', 'i32', 'f32'],
  ['f32.convert_i64_s', 'i64', 'f32'],
  ['f32.convert_i64_u', 'i64', 'f32'],
  ['f32.demote_f64', 'f64', 'f32'],
  ['f64.convert_i32_s', 'i32', 'f64'],
  ['f64.convert_i32_u', 'i32', 'f64'],
  ['f64.convert_i64_s', 'i64', 'f64'],
  ['f64.convert_i64_u', 'i64', 'f64'],
  ['f64.promote_f32', 'f32', 'f64'],
  ['i32.reinterpret_f32', 'f32', 'i32'],
  ['i64.reinterpret_f64', 'f64', 'i64'],
  ['f32.reinterpret_i32', 'i32', 'f32'],
  ['f64.reinterpret_i64', 'i64', 'f64'],
  ['i32.trunc_sat_f32_s', 'f32', 'i32'],
  ['i32.trunc_sat_f32_u', 'f32', 'i32'],
  ['i32.trunc_sat_f64_s', 'f64', 'i32'],
  ['i32.trunc_sat_f64_u', 'f64', 'i32'],
  ['i64.trunc_sat_f32_s', 'f32', 'i64'],
  ['i64.trunc_sat_f32_u', 'f32', 'i64'],
  ['i64.trunc_sat_f64_s', 'f64', 'i64'],
  ['i64.trunc_sat_f64_u', 'f64', 'i64'],
])
  add(n, [a], r);

// ---- SIMD operators ----------------------------------------------------------------------------------------------
const sops = [];
const A = '(v128.load (i32.const 0))',
  B = '(v128.load offset=16 (i32.const 0))',
  C = '(v128.load offset=32 (i32.const 0))';
const store = (e) => `(v128.store offset=64 (i32.const 0) ${e})`;
const un = (op) => sops.push({ n: op, body: store(`(${op} ${A})`) });
const bin = (op) => sops.push({ n: op, body: store(`(${op} ${A} ${B})`) });
const test = (op) => sops.push({ n: op, ret: 'i32', body: `(${op} ${A})` });
const shift = (op) => sops.push({ n: op, body: store(`(${op} ${A} (local.get 0))`) });
const cmp = ['eq', 'ne', 'lt_s', 'lt_u', 'gt_s', 'gt_u', 'le_s', 'le_u', 'ge_s', 'ge_u'];
const words = (s) => s.split(/\s+/).filter(Boolean);
words(`i8x16.abs i8x16.neg i8x16.popcnt i16x8.abs i16x8.neg i16x8.extend_low_i8x16_s i16x8.extend_high_i8x16_s i16x8.extend_low_i8x16_u i16x8.extend_high_i8x16_u i16x8.extadd_pairwise_i8x16_s i16x8.extadd_pairwise_i8x16_u
i32x4.abs i32x4.neg i32x4.extend_low_i16x8_s i32x4.extend_high_i16x8_s i32x4.extend_low_i16x8_u i32x4.extend_high_i16x8_u i32x4.extadd_pairwise_i16x8_s i32x4.extadd_pairwise_i16x8_u i32x4.trunc_sat_f32x4_s i32x4.trunc_sat_f32x4_u i32x4.trunc_sat_f64x2_s_zero i32x4.trunc_sat_f64x2_u_zero
i64x2.abs i64x2.neg i64x2.extend_low_i32x4_s i64x2.extend_high_i32x4_s i64x2.extend_low_i32x4_u i64x2.extend_high_i32x4_u
f32x4.abs f32x4.neg f32x4.sqrt f32x4.ceil f32x4.floor f32x4.trunc f32x4.nearest f32x4.convert_i32x4_s f32x4.convert_i32x4_u f32x4.demote_f64x2_zero
f64x2.abs f64x2.neg f64x2.sqrt f64x2.ceil f64x2.floor f64x2.trunc f64x2.nearest f64x2.promote_low_f32x4 f64x2.convert_low_i32x4_s f64x2.convert_low_i32x4_u v128.not`).forEach(
  un
);
const binl = [];
for (const s of ['i8x16', 'i16x8', 'i32x4']) for (const c of cmp) binl.push(`${s}.${c}`);
binl.push('i64x2.eq', 'i64x2.ne', 'i64x2.lt_s', 'i64x2.gt_s', 'i64x2.le_s', 'i64x2.ge_s');
for (const s of ['f32x4', 'f64x2']) for (const c of ['eq', 'ne', 'lt', 'gt', 'le', 'ge']) binl.push(`${s}.${c}`);
binl.push(
  ...words(`i8x16.add i8x16.add_sat_s i8x16.add_sat_u i8x16.sub i8x16.sub_sat_s i8x16.sub_sat_u i8x16.min_s i8x16.min_u i8x16.max_s i8x16.max_u i8x16.avgr_u i8x16.narrow_i16x8_s i8x16.narrow_i16x8_u i8x16.swizzle
i16x8.add i16x8.add_sat_s i16x8.add_sat_u i16x8.sub i16x8.sub_sat_s i16x8.sub_sat_u i16x8.mul i16x8.min_s i16x8.min_u i16x8.max_s i16x8.max_u i16x8.avgr_u i16x8.q15mulr_sat_s i16x8.narrow_i32x4_s i16x8.narrow_i32x4_u i16x8.extmul_low_i8x16_s i16x8.extmul_high_i8x16_s i16x8.extmul_low_i8x16_u i16x8.extmul_high_i8x16_u
i32x4.add i32x4.sub i32x4.mul i32x4.min_s i32x4.min_u i32x4.max_s i32x4.max_u i32x4.dot_i16x8_s i32x4.extmul_low_i16x8_s i32x4.extmul_high_i16x8_s i32x4.extmul_low_i16x8_u i32x4.extmul_high_i16x8_u
i64x2.add i64x2.sub i64x2.mul i64x2.extmul_low_i32x4_s i64x2.extmul_high_i32x4_s i64x2.extmul_low_i32x4_u i64x2.extmul_high_i32x4_u
f32x4.add f32x4.sub f32x4.mul f32x4.div f32x4.min f32x4.max f32x4.pmin f32x4.pmax f64x2.add f64x2.sub f64x2.mul f64x2.div f64x2.min f64x2.max f64x2.pmin f64x2.pmax v128.and v128.andnot v128.or v128.xor`)
);
binl.forEach(bin);
[
  'v128.any_true',
  'i8x16.all_true',
  'i8x16.bitmask',
  'i16x8.all_true',
  'i16x8.bitmask',
  'i32x4.all_true',
  'i32x4.bitmask',
  'i64x2.all_true',
  'i64x2.bitmask',
].forEach(test);
for (const s of ['i8x16', 'i16x8', 'i32x4', 'i64x2']) for (const o of ['shl', 'shr_s', 'shr_u']) shift(`${s}.${o}`);
sops.push({ n: 'v128.bitselect', body: store(`(v128.bitselect ${A} ${B} ${C})`) });
const lanes = { i8x16: 16, i16x8: 8, i32x4: 4, i64x2: 2, f32x4: 4, f64x2: 2 };
const sc = { i8x16: 'i32', i16x8: 'i32', i32x4: 'i32', i64x2: 'i64', f32x4: 'f32', f64x2: 'f64' };
for (const s in lanes) {
  sops.push({ n: `${s}.splat`, body: store(`(${s}.splat (${sc[s]}.load (i32.const 0)))`) });
  for (let l = 0; l < lanes[s]; l++) {
    for (const e of s === 'i8x16' || s === 'i16x8' ? ['extract_lane_s', 'extract_lane_u'] : ['extract_lane'])
      sops.push({ n: `${s}.${e}@${l}`, ret: sc[s], body: `(${s}.${e} ${l} ${A})` });
    sops.push({
      n: `${s}.replace_lane@${l}`,
      body: store(`(${s}.replace_lane ${l} ${A} (${sc[s]}.load offset=16 (i32.const 0)))`),
    });
  }
}
let seed = 12345;
const rnd = () => {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  return seed;
};
for (let t = 0; t < 12; t++) {
  const l = [];
  for (let i = 0; i < 16; i++) l.push(t < 2 ? (t ? i + 16 : i) : rnd() % 32);
  sops.push({ n: `shuffle@${l.join(',')}`, body: store(`(i8x16.shuffle ${l.join(' ')} ${A} ${B})`) });
}
sops.push({ n: 'v128.const', body: store('(v128.const i32x4 0x01020304 0x80000000 -1 0x7fffffff)') });
for (const o of [
  'load',
  'load8x8_s',
  'load8x8_u',
  'load16x4_s',
  'load16x4_u',
  'load32x2_s',
  'load32x2_u',
  'load8_splat',
  'load16_splat',
  'load32_splat',
  'load64_splat',
  'load32_zero',
  'load64_zero',
])
  sops.push({ n: 'v128.' + o, body: store(`(v128.${o} offset=3 (i32.const 1))`) });
for (const [o, n] of [
  ['load8_lane', 16],
  ['load16_lane', 8],
  ['load32_lane', 4],
  ['load64_lane', 2],
])
  for (let l = 0; l < n; l += Math.max(1, n >> 2))
    sops.push({ n: `v128.${o}@${l}`, body: store(`(v128.${o} offset=5 ${l} (i32.const 1) ${A})`) });
for (const [o, n] of [
  ['store8_lane', 16],
  ['store16_lane', 8],
  ['store32_lane', 4],
  ['store64_lane', 2],
])
  for (let l = 0; l < n; l += Math.max(1, n >> 2))
    sops.push({ n: `v128.${o}@${l}`, body: `${store(A)} (v128.${o} offset=70 ${l} (i32.const 0) ${B})` });

const tmp = require('os').tmpdir();
function build(name, wat) {
  const f = path.join(tmp, 'owos_' + name + '.wat');
  fs.writeFileSync(f, wat);
  return wasmOf(f);
}
const numWat =
  '(module\n' +
  nops
    .map(
      (o, i) =>
        `(func (export "f${i}") ${o.args.map((a) => `(param ${a})`).join(' ')} (result ${o.res}) ${o.args.map((a, k) => `local.get ${k}`).join(' ')} ${o.name})`
    )
    .join('\n') +
  ')\n';
const simdWat =
  '(module (memory (export "m") 1)\n' +
  sops.map((o, i) => `(func (export "f${i}") (param i32) ${o.ret ? `(result ${o.ret})` : ''} ${o.body})`).join('\n') +
  ')\n';

const mods = {};
for (const f of fs.readdirSync(dir).filter((f) => f.endsWith('.wat'))) mods[f.slice(0, -4)] = wasmOf(path.join(dir, f));
mods.num = build('num', numWat);
mods.simd = build('simd', simdWat);

const F32S = [
  0,
  -0,
  1,
  -1,
  0.5,
  -0.5,
  1.5,
  2.5,
  -2.5,
  3.7,
  NaN,
  Infinity,
  -Infinity,
  1e10,
  -1e10,
  2147483647.9,
  2147483648,
  4294967296,
  1e-40,
  3e38,
  -3e38,
  0.1,
  100.5,
  -7.25,
].map(hexf);
const F64S = [
  0,
  -0,
  1,
  -1,
  0.5,
  -0.5,
  1.5,
  2.5,
  -2.5,
  3.7,
  NaN,
  Infinity,
  -Infinity,
  1e10,
  -1e10,
  2147483647.9,
  -2147483648.9,
  4294967295.5,
  4294967296,
  9.3e18,
  -9.3e18,
  1.8e19,
  1e300,
  0.1,
].map(hexf);

// The script is data.js followed by script.js. Only the data is generated (into data.js); wasm_test.c reads both
// files from disk.
const hexOf = (b) => b.toString('hex');
// Decodes the module bytes in the data; defined with them so data.js stands on its own
const HEX = `function hex(s) {
  const u = new Uint8Array(s.length / 2);
  for (let i = 0; i < u.length; i++) u[i] = parseInt(s.substr(i * 2, 2), 16);
  return u;
}
`;
function data(E, H, recording) {
  let s =
    HEX +
    `const RECORDING = ${recording};\nconst E = ${JSON.stringify(E)};\nconst H = ${JSON.stringify(H)};\nconst W = {};\n`;
  for (const k of Object.keys(mods)) s += `W[${JSON.stringify(k)}] = hex(${JSON.stringify(hexOf(mods[k]))});\n`;
  s += `const NUM_OPS = ${JSON.stringify(nops.map((o) => [o.name, o.args]))};\n`;
  s += `const NUM_IN = { i32: ${JSON.stringify(I32)}, i64: ${JSON.stringify(I64)}, f32: ${JSON.stringify(FHEX)}, f64: ${JSON.stringify(FHEX)} };\n`;
  s += `const SIMD_OPS = ${JSON.stringify(sops.map((o) => [o.n, o.ret || '']))};\n`;
  s += `const F32S = ${JSON.stringify(F32S)}, F64S = ${JSON.stringify(F64S)};\n`;
  return s;
}

const source = (name) => fs.readFileSync(path.join(dir, name), 'utf8');
function script(E, H, recording) {
  return data(E, H, recording) + source('script.js');
}

// Record on V8
global.gc = () => {};
const ctx = {
  SharedArrayBuffer,
  structuredClone,
  Response,
  gc() {},
  console,
  Math,
  WebAssembly,
  Uint8Array,
  Float32Array,
  Float64Array,
  BigUint64Array,
  BigInt,
  Number,
  String,
  Object,
  Array,
  Promise,
  parseInt,
  Error,
  TypeError,
  RangeError,
  DataView,
  Uint32Array,
};
vm.createContext(ctx);
vm.runInContext(script({}, {}, true) + '\nvar __E = E, __H = H;', ctx);
// Promise reactions are asynchronous on V8: wait for them before reading the recorded table
setTimeout(() => {
  const E = ctx.__E,
    H = ctx.__H;
  const final = script(E, H, false);
  // Verify the generated script passes on V8 itself
  const ctx2 = {
    SharedArrayBuffer,
    structuredClone,
    Response,
    gc() {},
    console,
    Math,
    WebAssembly,
    Uint8Array,
    Float32Array,
    Float64Array,
    BigUint64Array,
    BigInt,
    Number,
    String,
    Object,
    Array,
    Promise,
    parseInt,
    Error,
    TypeError,
    RangeError,
    DataView,
    Uint32Array,
  };
  vm.createContext(ctx2);
  vm.runInContext(final, ctx2);
  setTimeout(() => {
    if (ctx2.__result !== '') {
      console.error('V8 disagrees with its own recording:\n' + ctx2.__result);
      process.exit(1);
    }
    const out = '// Generated by tests/wasm/gen.js, do not edit. script.js follows it.\n' + data(E, H, false);
    fs.writeFileSync(path.join(dir, 'data.js'), out);
    console.log(
      'wrote tests/wasm/data.js (' +
        out.length +
        ' bytes, ' +
        Object.keys(E).length +
        ' api checks, ' +
        Object.keys(H).length +
        ' operator hashes)'
    );
  }, 200);
}, 200);
