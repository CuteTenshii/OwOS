function fnv(s) {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return (h >>> 0).toString(16);
}
function chk(label, str) {
  const h = fnv(str);
  if (RECORDING) H[label] = h;
  else if (H[label] !== h) failures.push(label + ': results differ');
}
const failures = [];
function fmt(v) {
  if (typeof v === 'bigint') return v + 'n';
  if (Object.is(v, -0)) return '-0';
  if (typeof v === 'function') return 'fn:' + v.name;
  if (v === null) return 'null';
  if (Array.isArray(v)) return '[' + v.map(fmt).join(',') + ']';
  return String(v);
}
function T(label, f) {
  let r;
  gc();
  try {
    r = fmt(f());
  } catch (e) {
    r = 'throw:' + (e && e.name);
  }
  if (RECORDING) E[label] = r;
  else if (E[label] !== r) failures.push(label + ': got ' + r + ' want ' + E[label]);
}
function inst(name, imp) {
  gc();
  return new WebAssembly.Instance(new WebAssembly.Module(W[name]), imp || {}).exports;
}
const c = inst('ctl');
T('sum', function () {
  return [c.sum(0), c.sum(1), c.sum(10), c.sum(1000)];
});
T('sw', function () {
  return [0, 1, 2, 3, 4, 100, -1].map(c.sw);
});
T('swap', function () {
  return c.swap(1, 2);
});
T('mvblock', function () {
  return c.mvblock(3, 4);
});
T('loopparam', function () {
  return c.loopparam(5);
});
T('brval', function () {
  return [c.brval(0), c.brval(1)];
});
T('ifelse', function () {
  return [c.ifelse(0), c.ifelse(5)];
});
T('ifnoelse', function () {
  return [c.ifnoelse(0), c.ifnoelse(1)];
});
T('dead', function () {
  return [c.dead(), c.dead2(0), c.dead2(1)];
});
T('sel', function () {
  return [c.sel(0), c.sel(1), c.sel64(0), c.sel64(1)];
});
T('i64mix', function () {
  return c.i64mix(5n, -2, 0.5);
});
T('i64mix-bad', function () {
  return c.i64mix(5, -2, 0.5);
});
T('even', function () {
  return [c.even(0), c.even(7), c.even(100)];
});
T('rec', function () {
  return c.rec(0);
});
T('unr', function () {
  return c.unr();
});
T('retearly', function () {
  return [c.retearly(0), c.retearly(1)];
});
T('brif_ret', function () {
  return [c.brif_ret(0), c.brif_ret(1)];
});
T('v128loop', function () {
  return c.v128loop(5);
});
T('tee', function () {
  return c.tee(21);
});
T('nested', function () {
  return [c.nested(0), c.nested(1)];
});
T('ctl-props', function () {
  return [
    typeof c.sum,
    c.sum.name,
    c.sum.length,
    c.swap.length,
    Object.keys(c).length,
    Object.isFrozen(c),
    Object.getPrototypeOf(c),
  ];
});
const m = inst('mem'),
  U = new Uint8Array(m.mem.buffer);
T('data', function () {
  return [U[8], U[9], U[17], m.ld(8, 6)];
});
T('ld', function () {
  const r = [];
  for (let k = 0; k < 13; k++) r.push(m.ld(16, k));
  return r;
});
T('st', function () {
  m.st64(100, 0x0102030405060708n);
  m.st32(120, -2);
  m.st16(130, 0xabcd);
  m.st8(140, 0x1ff);
  return [m.ld(100, 6), m.ld(120, 5), m.ld(130, 3), m.ld(140, 1)];
});
T('stf', function () {
  m.stf(300, 1.5, -2.25);
  return [m.ldf(300)];
});
T('oob', function () {
  return m.ld(65536, 6);
});
T('oob2', function () {
  return m.ld(65529, 6);
});
T('oob-ok', function () {
  return m.ld(65528, 6);
});
T('off', function () {
  return m.off(0);
});
T('size', function () {
  return [m.size(), m.mem.buffer.byteLength];
});
T('grow', function () {
  const b = m.mem.buffer,
    r = m.grow(1);
  return [r, m.size(), b.byteLength, m.mem.buffer.byteLength, b === m.mem.buffer];
});
T('grow-same', function () {
  return m.mem.buffer === m.mem.buffer;
});
T('grow-fail', function () {
  return [m.grow(10), m.size()];
});
T('grow-js', function () {
  const b = m.mem.buffer;
  const r = m.mem.grow(1);
  return [r, b.byteLength, m.mem.buffer.byteLength, m.size()];
});
T('grow-js-fail', function () {
  return m.mem.grow(100);
});
T('grow-js0', function () {
  const b = m.mem.buffer;
  m.mem.grow(0);
  return [b.byteLength, m.mem.buffer.byteLength];
});
T('view-after-grow', function () {
  const u = new Uint8Array(m.mem.buffer);
  m.grow(0);
  const u2 = new Uint8Array(m.mem.buffer);
  return [u.length, u2.length, u2[8]];
});
T('fill', function () {
  m.fill(1000, 0xab, 10);
  return [m.ld(1000, 0), m.ld(1009, 0), m.ld(1010, 0)];
});
T('copy', function () {
  m.st64(2000, 0x1122334455667788n);
  m.copy(2002, 2000, 6);
  return [m.ld(2000, 6)];
});
T('copy-oob', function () {
  m.copy(0, 65536 * 3 - 4, 8);
});
T('fill-oob', function () {
  m.fill(65536 * 3 - 4, 0, 8);
});
T('init', function () {
  m.init(3000, 0, 11);
  return [m.ld(3000, 0), m.ld(3010, 0)];
});
T('init-oob', function () {
  m.init(3000, 5, 11);
});
T('drop-init', function () {
  m.drop();
  m.init(3000, 0, 1);
});
T('drop-init0', function () {
  m.init(3000, 0, 0);
  return 'ok';
});
T('last', function () {
  return m.last();
});
const mem2 = new WebAssembly.Memory({ initial: 1, maximum: 3 });
T('Memory', function () {
  return [
    mem2.buffer.byteLength,
    mem2.grow(1),
    mem2.buffer.byteLength,
    String(mem2),
    Object.prototype.toString.call(mem2),
    mem2 instanceof WebAssembly.Memory,
  ];
});
T('Memory-bad', function () {
  return new WebAssembly.Memory({});
});
T('Memory-max', function () {
  return new WebAssembly.Memory({ initial: 3, maximum: 2 });
});
T('Memory0', function () {
  const z = new WebAssembly.Memory({ initial: 0 });
  return [z.buffer.byteLength, z.grow(1), z.buffer.byteLength];
});
const t = inst('tbl');
T('call', function () {
  return [t.call(1, 5), t.call(2, 5), t.call(3, 0)];
});
T('call-null', function () {
  return t.call(0, 1);
});
T('call-sig', function () {
  return t.call(3, 1);
});
T('call-oob', function () {
  return t.call(100, 1);
});
T('callv', function () {
  return [t.callv(3)];
});
T('callv-sig', function () {
  return t.callv(1);
});
T('tsize', function () {
  return [t.size(), t.t.length];
});
T('tget', function () {
  return [t.t.get(1) === t.t.get(1), typeof t.t.get(1), t.t.get(0), t.t.get(1)(5), t.t.get(2)(5)];
});
T('tgrow', function () {
  const r = t.grow(3);
  return [r, t.size(), t.t.length, t.grow(100)];
});
T('isnull', function () {
  return [t.isnull(0), t.isnull(1)];
});
T('set_inc', function () {
  t.set_inc(0);
  return [t.isnull(0), t.call(0, 9)];
});
T('set_null', function () {
  t.set_null(0);
  return t.isnull(0);
});
T('tfill', function () {
  t.fill(4, 2);
  return [t.call(4, 6), t.call(5, 6)];
});
T('tinit', function () {
  t.init(0, 0, 3);
  return [t.call(0, 1), t.isnull(1), t.call(2, 10)];
});
T('tinit-oob', function () {
  t.init(0, 2, 3);
});
T('tcopy', function () {
  t.tcopy(10, 0, 2);
  return [t.call(10, 1), t.call(11, 1)];
});
T('edrop', function () {
  t.edrop();
  t.init(0, 0, 1);
});
T('getfn', function () {
  const f = t.getfn(1);
  return [typeof f, f(3), f === t.getfn(1), f === t.t.get(1), t.getfn(100)];
});
T('reffn', function () {
  const f = t.reffn();
  return [f(), f === t.reffn(), f.name];
});
T('tset', function () {
  t.t.set(0, t.t.get(1));
  return t.call(0, 4);
});
T('tset-bad', function () {
  t.t.set(0, function () {});
});
T('tset-null', function () {
  t.t.set(0, null);
  return t.isnull(0);
});
T('tget-oob', function () {
  return t.t.get(1000);
});
T('tgrow-js', function () {
  return [t.t.grow(2), t.t.length];
});
const tj = new WebAssembly.Table({ element: 'anyfunc', initial: 2 });
T('Table', function () {
  return [tj.length, tj.get(0), tj.grow(1), tj.length, String(tj)];
});
T('Table-bad', function () {
  return new WebAssembly.Table({ element: 'i32', initial: 1 });
});
T('Table-set', function () {
  t.t.set(1, t.reffn());
  tj.set(0, t.t.get(1));
  return tj.get(0)();
});
const g = inst('glob', { env: { g: 7, mg: new WebAssembly.Global({ value: 'i32', mutable: true }, 5) } });
T('glob', function () {
  return [g.a.value, g.b.value, g.c.value, g.d.value, g.e.value, g.a instanceof WebAssembly.Global, String(g.a)];
});
T('globset', function () {
  g.b.value = 100n;
  g.incb();
  g.c.value = 3.5;
  g.d.value = -1e10;
  return [g.b.value, g.c.value, g.d.value];
});
T('glob-imm', function () {
  g.a.value = 1;
});
T('glob-i64-num', function () {
  g.b.value = 1;
});
T('mg', function () {
  const mg = new WebAssembly.Global({ value: 'i32', mutable: true }, 9);
  const g2 = inst('glob', { env: { g: 1, mg: mg } });
  g2.setmg(77);
  return [mg.value, g2.getmg()];
});
T('mg-bad', function () {
  return inst('glob', { env: { g: 1, mg: 5 } });
});
T('g-bad', function () {
  return inst('glob', { env: { g: {}, mg: new WebAssembly.Global({ value: 'i32', mutable: true }, 0) } });
});
T('g-mismatch', function () {
  return inst('glob', { env: { g: 1, mg: new WebAssembly.Global({ value: 'i64', mutable: true }, 0n) } });
});
T('vglob', function () {
  g.vset();
  return [g.vsum(), g.v.valueOf === undefined];
});
T('Global', function () {
  const x = new WebAssembly.Global({ value: 'i64' }, 5n);
  const y = new WebAssembly.Global({ value: 'f32', mutable: true });
  y.value = 0.1;
  return [x.value, y.value, x.valueOf()];
});
T('Global-ref', function () {
  const x = new WebAssembly.Global({ value: 'externref', mutable: true }, { a: 1 });
  return [
    x.value.a,
    new WebAssembly.Global({ value: 'anyfunc' }).value,
    new WebAssembly.Global({ value: 'externref' }).value,
  ];
});
const log = [];
const imp = {
  env: {
    log: function (x) {
      gc();
      log.push(x);
    },
    add: function (a, b) {
      gc();
      return a + b;
    },
    big: function (x) {
      return x * 2n;
    },
    fl: function (a, b) {
      return a + b;
    },
    pair: function (x) {
      return [x + 1, BigInt(x) * 10n];
    },
    boom: function () {
      throw new RangeError('boom!');
    },
    cb: null,
  },
};
let im;
T('imp-load', function () {
  imp.env.cb = function (x) {
    gc();
    return im.dbl(x) + 1;
  };
  im = inst('imp', imp);
  return 'ok';
});
T('t_log', function () {
  im.t_log(5);
  im.t_log(-1);
  im.log_many(4);
  return log;
});
T('t_add', function () {
  return [im.t_add(1, 2), im.t_add(2147483647, 1), im.t_add(1.9, '3')];
});
T('t_big', function () {
  return [im.t_big(21n), im.t_big(-5n)];
});
T('t_fl', function () {
  return im.t_fl(1.5, 2.25);
});
T('t_pair', function () {
  return im.t_pair(4);
});
T('t_boom', function () {
  im.t_boom();
});
T('t_boom-catch', function () {
  try {
    im.t_boom();
  } catch (e) {
    return [e instanceof RangeError, e.message];
  }
});
T('t_cb', function () {
  return [im.t_cb(5), im.loopcb(100)];
});
T('t_cb-throw', function () {
  imp.env.cb = function () {
    throw new Error('x');
  };
  try {
    im.t_cb(1);
  } catch (e) {
    imp.env.cb = function (x) {
      return im.dbl(x) + 1;
    };
    return e.message;
  }
});
T('imp-missing', function () {
  return inst('imp', {});
});
T('imp-nofn', function () {
  return inst('imp', { env: { log: 1 } });
});
T('imp-nomod', function () {
  return inst('imp', { other: {} });
});
T('imp-noobj', function () {
  return new WebAssembly.Instance(new WebAssembly.Module(W.imp));
});
T('imp-wasmfn', function () {
  inst('imp', {
    env: {
      log: function () {},
      add: c.swap,
      big: function (x) {
        return x;
      },
      fl: function () {
        return 0;
      },
      pair: function () {
        return [1, 1n];
      },
      boom: function () {},
      cb: im.dbl,
    },
  });
  return 'ok';
});
T('imp-wasmfn2', function () {
  const i2 = inst('imp', {
    env: {
      log: function () {},
      add: im.t_add,
      big: function (x) {
        return x;
      },
      fl: function () {
        return 0;
      },
      pair: function () {
        return [1, 1n];
      },
      boom: function () {},
      cb: im.dbl,
    },
  });
  return i2.t_add(40, 2);
});
T('pair-bad', function () {
  imp.env.pair = function () {
    return 5;
  };
  const i2 = inst('imp', imp);
  return i2.t_pair(1);
});
T('pair-len', function () {
  imp.env.pair = function () {
    return [1];
  };
  const i2 = inst('imp', imp);
  return i2.t_pair(1);
});
const x = inst('ext');
const o = { a: 1 };
T('ext-id', function () {
  return [
    x.id(o) === o,
    x.id(null),
    x.id(undefined),
    x.id(5),
    x.id('s'),
    x.id(true),
    x.id(1.5),
    x.isnull(null),
    x.isnull(undefined),
    x.isnull(0),
    x.isnull(o),
  ];
});
T('ext-table', function () {
  x.tset(0, o);
  x.tset(1, 'str');
  x.tset(2, null);
  return [x.tget(0) === o, x.tget(1), x.tget(2), x.tget(3), x.t.get(0) === o, x.t.get(3)];
});
T('ext-global', function () {
  x.gset(o);
  return [x.gget() === o, x.g.value === o, ((x.g.value = 7), x.gget())];
});
T('ext-grow', function () {
  return [x.grow(o, 2), x.t.length, x.tget(5) === o];
});
T('ext-fill', function () {
  x.fill(0, o, 3);
  return [x.tget(0) === o, x.tget(2) === o, x.tget(3)];
});
T('ext-pass3', function () {
  return x.pass3(1, 2, 3);
});
T('ext-loop', function () {
  return x.loop(o, 1000) === o;
});
T('ext-tjs', function () {
  x.t.set(0, 'q');
  x.t.set(1);
  return [x.tget(0), x.tget(1), x.t.get(1)];
});
T('ext-tnew', function () {
  const t2 = new WebAssembly.Table({ element: 'externref', initial: 2 }, o);
  return [t2.get(0) === o, t2.grow(1), t2.get(2), new WebAssembly.Table({ element: 'externref', initial: 1 }).get(0)];
});
const sl = [];
T('start', function () {
  const s = inst('start', {
    env: {
      note: function (v) {
        sl.push(v);
      },
    },
  });
  return [sl, s.get(), new Uint8Array(s.m.buffer)[0]];
});
T('trapstart', function () {
  return inst('trapstart');
});
T('badseg', function () {
  return inst('badseg');
});
const ex = new WebAssembly.Module(W.exports);
T('mod-exports', function () {
  return WebAssembly.Module.exports(ex).map(function (e) {
    return e.name + ':' + e.kind;
  });
});
T('mod-imports', function () {
  return WebAssembly.Module.imports(new WebAssembly.Module(W.imp))
    .map(function (e) {
      return e.module + '.' + e.name + ':' + e.kind;
    })
    .join(' ');
});
T('mod-custom', function () {
  const cs = WebAssembly.Module.customSections(ex, 'mysec');
  return [cs.length, cs[0].byteLength, WebAssembly.Module.customSections(ex, 'none').length];
});
T('exports-order', function () {
  return Object.keys(new WebAssembly.Instance(ex).exports);
});
T('exports-alias', function () {
  const e = new WebAssembly.Instance(ex).exports;
  return [e.alias === e.dup, e.alias.name, e.f.length, e.mem.buffer.byteLength, e.tab.length, e.g.value];
});
T('exports-frozen', function () {
  const e = new WebAssembly.Instance(ex).exports;
  e.f = 1;
  e.zz = 2;
  return [typeof e.f, e.zz];
});
T('validate', function () {
  return [
    WebAssembly.validate(W.ctl),
    WebAssembly.validate(new Uint8Array(4)),
    WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0])),
    WebAssembly.validate(W.ctl.buffer),
  ];
});
T('validate-bad', function () {
  return WebAssembly.validate(5);
});
T('module-bad', function () {
  return new WebAssembly.Module(5);
});
T('module-empty', function () {
  return new WebAssembly.Module(new Uint8Array(0));
});
T('module-trunc', function () {
  return new WebAssembly.Module(W.ctl.subarray(0, 50));
});
T('module-buf', function () {
  return new WebAssembly.Module(W.ctl.buffer.slice(0)) instanceof WebAssembly.Module;
});
T('instance-bad', function () {
  return new WebAssembly.Instance({});
});
T('types', function () {
  return [
    typeof WebAssembly,
    Object.prototype.toString.call(WebAssembly),
    typeof WebAssembly.Module,
    Object.prototype.toString.call(ex),
    new WebAssembly.CompileError('x').name,
    new WebAssembly.LinkError('y') instanceof Error,
    String(new WebAssembly.RuntimeError('z')),
  ];
});

const ec = inst('extconst', { env: { a: 10 } });
T('extconst', function () {
  return [ec.b.value, ec.c.value, ec.rd()];
});
const tc = inst('tailcall', {
  env: {
    h: function (x) {
      return x * 3;
    },
  },
});
T('tail-self', function () {
  return [tc.acc(10, 0), tc.acc(1000000, 0)];
});
T('tail-indirect', function () {
  return [tc.ind(10), tc.ind(500000)];
});
T('tail-host', function () {
  return tc.th(14);
});
T('tail-multi', function () {
  return tc.mv(5);
});
const rx = inst('relaxed');
T('relaxed-fp', function () {
  return [rx.madd(3, 4, 5), rx.nmadd(2, 3, 10), rx.fmin(1.5, 2.5), rx.dmax(-1, -2)];
});
T('relaxed-int', function () {
  return [
    rx.trunc(-7.9),
    rx.truncu(12.7),
    rx.lsel(0x0f0f0f0f, 0x33333333, -1),
    rx.lsel(5, 9, 0),
    rx.swz(3),
    rx.swz(200),
    rx.q15(16384, 16384),
    rx.dot(3, 5),
    rx.dotadd(2, 6, 100),
  ];
});
const mm = inst('multimem');
T('multimem', function () {
  return [
    mm.lda(0),
    mm.lda(1),
    mm.ldb(10),
    mm.ldb(12),
    mm.sza(),
    mm.szb(),
    mm.a.buffer.byteLength,
    mm.b.buffer.byteLength,
    mm.a === mm.b,
  ];
});
T('multimem-ops', function () {
  mm.stb(100, 77);
  mm.cp(200, 100, 1);
  mm.fillb(300, 9, 4);
  mm.initb(400, 1, 2);
  return [mm.lda(200), mm.ldb(300), mm.ldb(303), mm.ldb(304), mm.ldb(400), mm.ldb(401), mm.vb(300)];
});
T('multimem-grow', function () {
  const b = mm.b.buffer,
    a = mm.a.buffer,
    r = mm.grb(1);
  return [r, mm.szb(), b.byteLength, mm.b.buffer.byteLength, mm.sza(), a === mm.a.buffer];
});
T('multimem-grow-fail', function () {
  return [mm.grb(10), mm.szb()];
});
T('multimem-oob', function () {
  return mm.ldb(65536 * 3);
});
T('multimem-copy-oob', function () {
  mm.cp(0, 65530 + 65536 * 2, 100);
});
const m64 = inst('memory64');
T('mem64', function () {
  m64.st(5n, 0x01020304);
  return [m64.ld(8n), m64.ld(21n), m64.sz(), m64.m.buffer.byteLength];
});
T('mem64-grow', function () {
  const b = m64.m.buffer;
  return [
    m64.gr(1n),
    m64.sz(),
    b.byteLength,
    m64.m.buffer.byteLength,
    m64.gr(100n),
    m64.m.grow(1n),
    typeof m64.m.grow(0n),
    m64.sz(),
  ];
});
T('mem64-bulk', function () {
  m64.fill(100n, 0x5a, 8n);
  m64.cp(200n, 100n, 8n);
  return [m64.ld(200n), m64.ld(204n), m64.ldv(100n)];
});
T('mem64-oob', function () {
  return m64.far();
});
T('mem64-wrap', function () {
  return m64.wrap(2n);
});
T('mem64-i32-arg', function () {
  return m64.ld(8);
});
T('mem64-js', function () {
  const m = new WebAssembly.Memory({ initial: 1n, maximum: 2n, address: 'i64' });
  return [m.buffer.byteLength, m.grow(1n), m.buffer.byteLength, typeof m.grow(0n)];
});
T('mem64-js-num', function () {
  return new WebAssembly.Memory({ initial: 1, address: 'i64' });
});
T('mem64-import', function () {
  const m = new WebAssembly.Memory({ initial: 2n, maximum: 3n, address: 'i64' });
  return inst('mem64imp', { env: { m: m } }).sz();
});
T('mem64-import-bad', function () {
  return inst('mem64imp', { env: { m: new WebAssembly.Memory({ initial: 2 }) } });
});
const jt = new WebAssembly.Tag({ parameters: ['i32'] });
const jsErr = new Error('boom');
function jsthrow(x) {
  if (x === 1) throw jsErr;
  if (x === 2) throw new WebAssembly.Exception(jt, [77]);
  if (x === 3) throw 5;
}
const eh = inst('eh', { env: { jsthrow: jsthrow, jt: jt } });
function caught(f) {
  try {
    f();
    return 'no throw';
  } catch (e) {
    return e;
  }
}
T('eh-catch', function () {
  return [
    eh.catch0(5),
    eh.catchall(0),
    eh.catchall(1),
    eh.rethrow(4),
    eh.delegate(3),
    eh.under(5),
    eh.loopc(3),
    eh.brout(4),
    eh.deep(5),
  ];
});
T('eh-pair', function () {
  return eh.pair();
});
T('eh-escape', function () {
  const e = caught(function () {
    eh.nomatch(9);
  });
  return [e instanceof WebAssembly.Exception, e.is(eh.e0), e.is(eh.e1), e.getArg(eh.e0, 0), e instanceof Error];
});
T('eh-delegate-out', function () {
  const e = caught(function () {
    eh.delegate_out(3);
  });
  return [e instanceof WebAssembly.Exception, e.getArg(eh.e0, 0)];
});
T('eh-js', function () {
  return [eh.js(0), eh.js(1), eh.js(2), eh.js(3), eh.jstag(0), eh.jstag(2)];
});
T('eh-js-identity', function () {
  return [
    caught(function () {
      eh.jstag(1);
    }) === jsErr,
    caught(function () {
      eh.jsre(1);
    }) === jsErr,
    caught(function () {
      eh.jsre(3);
    }),
    caught(function () {
      eh.jsre(2);
    }) instanceof WebAssembly.Exception,
  ];
});
T('eh-trap', function () {
  return eh.trap();
});
T('eh-tags', function () {
  return [eh.e0 instanceof WebAssembly.Tag, eh.e0 === eh.e0, eh.e0 === eh.e1, Object.prototype.toString.call(eh.e0)];
});
const eh2 = inst('eh2');
T('eh2-basic', function () {
  return [eh2.tt(9), eh2.tt_all(0), eh2.tt_all(1), eh2.tt_ref(5), eh2.nested(5)];
});
T('eh2-mk', function () {
  const x = eh2.mk(7);
  return [x instanceof WebAssembly.Exception, x.is(eh2.e), x.getArg(eh2.e, 0), eh2.isnull(x), eh2.isnull(null)];
});
T('eh2-throw-ref', function () {
  const e = caught(function () {
    eh2.thr(eh2.mk(8));
  });
  return [e instanceof WebAssembly.Exception, e.is(eh2.e), e.getArg(eh2.e, 0)];
});
T('eh2-throw-ref-null', function () {
  eh2.thr(null);
});
T('eh2-bad-exnref', function () {
  eh2.thr({});
});
T('eh2-escape', function () {
  const e = caught(function () {
    eh2.nomatch(1);
  });
  return [e instanceof WebAssembly.Exception, e.is(eh2.e)];
});
T('Exception', function () {
  const x = new WebAssembly.Exception(jt, [3.9]);
  const y = new WebAssembly.Exception(eh.e1, []);
  return [x.is(jt), x.is(eh.e0), x.getArg(jt, 0), y.is(eh.e1), String(x), Object.prototype.toString.call(x)];
});
T('Exception-bad', function () {
  return new WebAssembly.Exception(jt, []);
});
T('Exception-wrong-tag', function () {
  return new WebAssembly.Exception(jt, [1]).getArg(eh.e0, 0);
});
T('Exception-index', function () {
  return new WebAssembly.Exception(jt, [1]).getArg(jt, 1);
});
T('Tag-bad', function () {
  return new WebAssembly.Tag({});
});
T('Tag-types', function () {
  return new WebAssembly.Tag({ parameters: ['i64', 'f32', 'externref'] }) instanceof WebAssembly.Tag;
});
T('tag-import-bad', function () {
  return inst('eh', { env: { jsthrow: jsthrow, jt: new WebAssembly.Tag({ parameters: [] }) } });
});
T('tag-import-nontag', function () {
  return inst('eh', { env: { jsthrow: jsthrow, jt: {} } });
});
T('tag-kinds', function () {
  return [
    WebAssembly.Module.exports(new WebAssembly.Module(W.eh)).filter(function (e) {
      return e.kind === 'tag';
    }).length,
    WebAssembly.Module.imports(new WebAssembly.Module(W.eh))
      .map(function (e) {
        return e.kind;
      })
      .join(),
  ];
});
const th = inst('threads');
T('atomic-mem', function () {
  const b = th.mem.buffer;
  return [
    b instanceof SharedArrayBuffer,
    b.byteLength,
    th.size(),
    th.mem.buffer === th.mem.buffer,
    Object.prototype.toString.call(th.mem),
  ];
});
T('atomic-ldst', function () {
  th.st32(0, 0x11223344);
  th.st8(8, 0x1ff);
  th.st16(10, 0xabcdef);
  th.st64(16, 0x0102030405060708n);
  th.st64_32(24, 0x1ffffffffn);
  return [
    th.ld32(0),
    th.ld8(0),
    th.ld16(0),
    th.ld8(8),
    th.ld16(10),
    th.ld64(16),
    th.ld64_32(16),
    th.ld64_32(24),
    th.ld64(24),
  ];
});
T('atomic-rmw', function () {
  th.st32(32, 10);
  th.st32(36, 0x1ff);
  th.st32(40, 0xf0f0f0f0);
  th.st64(48, 5n);
  th.st64(56, 0xffffffff00000001n);
  th.st32(64, 7);
  th.st64(72, 0x1122334455667788n);
  return [
    th.add32(32, 5),
    th.ld32(32),
    th.sub8(36, 2),
    th.ld32(36),
    th.and16(40, 0xff00),
    th.ld32(40),
    th.or64(48, 0xf0n),
    th.ld64(48),
    th.xor64_32(56, 0xffn),
    th.ld64(56),
    th.xchg32(64, 99),
    th.ld32(64),
    th.xchg64_16(72, 0xabcdn),
    th.ld64(72),
  ];
});
T('atomic-cas', function () {
  th.st32(80, 5);
  th.st64(88, 9n);
  th.st32(96, 0x100);
  return [
    th.cas32(80, 5, 6),
    th.ld32(80),
    th.cas32(80, 5, 7),
    th.ld32(80),
    th.cas64(88, 9n, 10n),
    th.ld64(88),
    th.cas64(88, 9n, 11n),
    th.cas8(96, 0x100, 0x77),
    th.ld32(96),
    th.cas8(96, 0, 0x77),
    th.ld32(96),
  ];
});
T('atomic-js-view', function () {
  const i32 = new Int32Array(th.mem.buffer);
  Atomics.store(i32, 30, 1234);
  th.st32(124, 55);
  return [th.ld32(120), i32[31], Atomics.add(i32, 30, 1), th.ld32(120), Atomics.load(i32, 31)];
});
T('atomic-notify', function () {
  return [th.notify(0, 10), th.notify(4, 0)];
});
T('atomic-wait', function () {
  th.st32(200, 3);
  th.st64(208, 4n);
  return [th.wait32(200, 9, 0n), th.wait32(200, 3, 1000000n), th.wait64(208, 5n, 0n), th.wait64(208, 4n, 1000000n)];
});
T('atomic-fence', function () {
  th.fence();
  return 'ok';
});
T('atomic-unaligned', function () {
  return th.odd();
});
T('atomic-unaligned64', function () {
  return th.odd64();
});
T('atomic-oob', function () {
  return th.oob();
});
T('atomic-grow', function () {
  const b = th.mem.buffer,
    r = th.grow(1);
  const b2 = th.mem.buffer;
  return [
    r,
    th.size(),
    b.byteLength,
    b2.byteLength,
    b === b2,
    b2 instanceof SharedArrayBuffer,
    th.grow(10),
    th.mem.grow(1),
    th.mem.buffer.byteLength,
  ];
});
T('atomic-grow-visible', function () {
  const i32 = new Int32Array(th.mem.buffer);
  th.st32(70000, 5);
  return [i32.length * 4, i32[70000 / 4], th.ld32(70000)];
});
const ap = inst('atomicpriv');
T('atomic-private', function () {
  return [ap.add(0, 3), ap.add(0, 4), ap.ld(0), ap.notify()];
});
T('atomic-private-wait', function () {
  return ap.wait();
});
T('shared-memory-js', function () {
  const m = new WebAssembly.Memory({ initial: 1, maximum: 2, shared: true });
  const b = m.buffer;
  const g = m.grow(1);
  return [b instanceof SharedArrayBuffer, b.byteLength, g, m.buffer.byteLength, m.buffer !== b];
});
T('shared-memory-nomax', function () {
  return new WebAssembly.Memory({ initial: 1, shared: true });
});
T('shared-import', function () {
  const m = new WebAssembly.Memory({ initial: 1, maximum: 2, shared: true });
  const s = inst('sharedimp', { env: { m: m } });
  s.add(0, 5);
  return [s.ld(0), new Int32Array(m.buffer)[0], s.size()];
});
T('shared-import-private', function () {
  return inst('sharedimp', { env: { m: new WebAssembly.Memory({ initial: 1, maximum: 2 }) } });
});
T('shared-import-private2', function () {
  return (
    inst('sharedimp', { env: { m: new WebAssembly.Memory({ initial: 1, maximum: 2, shared: true }) } }) &&
    inst('threads').mem.buffer.byteLength
  );
});
T('clone-module', function () {
  const m = new WebAssembly.Module(W.ctl);
  const c = structuredClone(m);
  return [c instanceof WebAssembly.Module, c !== m, new WebAssembly.Instance(c).exports.sum(4)];
});
T('clone-shared-memory', function () {
  const m = new WebAssembly.Memory({ initial: 1, maximum: 2, shared: true });
  const c = structuredClone(m);
  new Int32Array(m.buffer)[3] = 42;
  return [
    c instanceof WebAssembly.Memory,
    c !== m,
    new Int32Array(c.buffer)[3],
    c.buffer instanceof SharedArrayBuffer,
    c.grow(1),
    m.buffer.byteLength,
  ];
});
T('clone-private-memory', function () {
  return structuredClone(new WebAssembly.Memory({ initial: 1 }));
});
T('clone-instance', function () {
  return structuredClone(new WebAssembly.Instance(new WebAssembly.Module(W.ctl)));
});
function dbl(h) {
  const b = new BigUint64Array(1),
    d = new Float64Array(b.buffer);
  b[0] = BigInt('0x' + h);
  return d[0];
}
function big(s) {
  return s[0] === '-' ? -BigInt(s.slice(1)) : BigInt(s);
}
function numArg(t, s) {
  return t === 'i32' ? Number(s) | 0 : t === 'i64' ? big(s) : dbl(s);
}
function runNum() {
  const X = new WebAssembly.Instance(new WebAssembly.Module(W.num), {}).exports;
  for (let i = 0; i < NUM_OPS.length; i++) {
    const out = [],
      a = NUM_OPS[i][1],
      f = X['f' + i];
    const go = function (args) {
      try {
        out.push(fmt(args.length === 1 ? f(args[0]) : f(args[0], args[1])));
      } catch (e) {
        out.push('trap:' + e.name);
      }
    };
    if (a.length === 1)
      NUM_IN[a[0]].forEach(function (x) {
        go([numArg(a[0], x)]);
      });
    else
      NUM_IN[a[0]].forEach(function (x) {
        NUM_IN[a[1]].forEach(function (y) {
          go([numArg(a[0], x), numArg(a[1], y)]);
        });
      });
    chk('num:' + NUM_OPS[i][0], out.join(','));
  }
}
function runSimd() {
  const X = new WebAssembly.Instance(new WebAssembly.Module(W.simd), {}).exports,
    M = new Uint8Array(X.m.buffer);
  let s = 7;
  function rnd() {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    return s;
  }
  function toArr(u) {
    const r = [];
    for (let i = 0; i < u.length; i++) r.push(u[i]);
    return r;
  }
  function mk(f) {
    const v = [];
    for (let i = 0; i < 16; i++) v.push(f(i));
    return v;
  }
  function fvec32() {
    const a = new Float32Array(4);
    for (let i = 0; i < 4; i++) a[i] = dbl(F32S[rnd() % F32S.length]);
    return toArr(new Uint8Array(a.buffer));
  }
  function fvec64() {
    const a = new Float64Array(2);
    for (let i = 0; i < 2; i++) a[i] = dbl(F64S[rnd() % F64S.length]);
    return toArr(new Uint8Array(a.buffer));
  }
  const V = [
    mk(function () {
      return 0;
    }),
    mk(function () {
      return 255;
    }),
    mk(function () {
      return 128;
    }),
    mk(function () {
      return 127;
    }),
    mk(function (i) {
      return i;
    }),
    mk(function (i) {
      return (i * 17) & 255;
    }),
  ];
  for (let i = 0; i < 4; i++)
    V.push(
      mk(function () {
        return rnd() & 255;
      })
    );
  for (let i = 0; i < 4; i++) V.push(fvec32());
  for (let i = 0; i < 3; i++) V.push(fvec64());
  function canon(bytes, mode) {
    let u = new Uint8Array(bytes),
      w = new Uint32Array(u.buffer),
      i;
    if (mode === 'f32') {
      const f = new Float32Array(u.buffer);
      for (i = 0; i < 4; i++) if (f[i] !== f[i]) w[i] = 0x7fc00000;
    }
    if (mode === 'f64') {
      const g = new Float64Array(u.buffer);
      for (i = 0; i < 2; i++)
        if (g[i] !== g[i]) {
          w[2 * i] = 0;
          w[2 * i + 1] = 0x7ff80000;
        }
    }
    return toArr(u);
  }
  function hex(b) {
    return b
      .map(function (x) {
        return (x < 16 ? '0' : '') + x.toString(16);
      })
      .join('');
  }
  const tops = [0, 1, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65];
  for (let i = 0; i < SIMD_OPS.length; i++) {
    let f = X['f' + i],
      name = SIMD_OPS[i][0],
      ret = SIMD_OPS[i][1],
      out = [],
      mode = '';
    if (/^f32x4\.(?!extract|eq|ne|lt|gt|le|ge)/.test(name)) mode = 'f32';
    if (/^f64x2\.(?!extract|eq|ne|lt|gt|le|ge)/.test(name)) mode = 'f64';
    if (/^f32x4\.convert_i/.test(name)) mode = 'f32';
    if (/^f64x2\.(convert|promote)/.test(name)) mode = 'f64';
    const shifts = /x.*\.(shl|shr)/.test(name) ? tops : [0];
    const nargs =
      /^v128\.(any|bitselect)|swizzle|all_true|bitmask|\.(abs|neg|not|popcnt|sqrt|ceil|floor|trunc|nearest|extend|extadd|trunc_sat|convert|demote|promote)/.test(
        name
      )
        ? 1
        : 2;
    let pairs = [];
    for (let a = 0; a < V.length; a++) for (let b = 0; b < (nargs === 2 ? V.length : 1); b++) pairs.push([a, b]);
    if (/bitselect/.test(name))
      pairs = pairs.filter(function (p, k) {
        return k % 5 === 0;
      });
    for (let pi = 0; pi < pairs.length; pi++) {
      for (let si = 0; si < shifts.length; si++) {
        if (si > 0 && pi > 6) break;
        M.set(V[pairs[pi][0]], 0);
        M.set(V[pairs[pi][1]], 16);
        M.set(V[(pairs[pi][0] + 3) % V.length], 32);
        M.fill(0, 64, 96);
        try {
          const r = f(shifts[si]);
          out.push(ret ? fmt(r) : hex(canon(toArr(M.subarray(64, 96)), mode)));
        } catch (e) {
          out.push('trap:' + e.name);
        }
      }
    }
    chk('simd:' + name, out.join(','));
  }
}

runNum();
runSimd();
function fuzz(name, n) {
  let s = 12345,
    base = W[name];
  for (let i = 0; i < n; i++) {
    let m = new Uint8Array(base.length),
      k;
    m.set(base);
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    k = 1 + (s % 3);
    for (let j = 0; j < k; j++) {
      s = (s * 1103515245 + 12345) & 0x7fffffff;
      m[8 + (s % (m.length - 8))] = (s >> 8) & 255;
    }
    try {
      WebAssembly.validate(m);
    } catch (e) {
      // a rejected mutation is the expected outcome; only a crash matters
    }
  }
}
['ctl', 'mem', 'tbl', 'imp', 'glob', 'ext', 'exports'].forEach(function (n) {
  fuzz(n, 600);
});
const order = [];
function Resp(buf) {
  this.arrayBuffer = function () {
    return Promise.resolve(buf);
  };
}
function resp(buf) {
  return typeof Response === 'function'
    ? new Response(buf, { headers: { 'Content-Type': 'application/wasm' } })
    : new Resp(buf);
}
const pending = [
  WebAssembly.instantiate(W.ctl).then(function (r) {
    order.push(
      'inst:' +
        Object.keys(r).join('/') +
        ':' +
        r.instance.exports.sum(4) +
        ':' +
        (r.module instanceof WebAssembly.Module)
    );
  }),
  WebAssembly.instantiate(new WebAssembly.Module(W.exports)).then(function (r) {
    order.push('inst2:' + (r instanceof WebAssembly.Instance));
  }),
  WebAssembly.compile(W.ctl).then(function (m) {
    order.push('compile:' + (m instanceof WebAssembly.Module));
  }),
  WebAssembly.compile(new Uint8Array(3)).then(null, function (e) {
    order.push('compile-bad:' + e.name);
  }),
  WebAssembly.instantiate(W.imp, {}).then(null, function (e) {
    order.push('link:' + e.name);
  }),
  WebAssembly.instantiate(5).then(null, function (e) {
    order.push('type:' + e.name);
  }),
  WebAssembly.instantiateStreaming(resp(W.ctl.buffer), {}).then(function (r) {
    order.push('stream:' + r.instance.exports.sum(5));
  }),
  WebAssembly.compileStreaming(Promise.resolve(resp(W.ctl.buffer))).then(function (m) {
    order.push('cstream:' + (m instanceof WebAssembly.Module));
  }),
];
Promise.all(pending).then(function () {
  const got = order.slice().sort().join('|');
  if (RECORDING) E['promises'] = got;
  else if (E['promises'] !== got) failures.push('promises: got ' + got + ' want ' + E['promises']);
  __result = failures.join('\n');
});
