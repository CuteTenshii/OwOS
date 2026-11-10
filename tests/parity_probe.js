/* exported PROBE */
function PROBE() {
  var o = {},
    jobs = [];
  function s(v) {
    return typeof v === 'object' && v !== null ? JSON.stringify(v) : String(v);
  }
  function t(k, f) {
    try {
      o[k] = s(f());
    } catch (e) {
      o[k] = 'throw ' + (e && e.name);
    }
  }
  function ta(k, f) {
    jobs.push(
      Promise.resolve()
        .then(f)
        .then(
          function (v) {
            o[k] = s(v);
          },
          function (e) {
            o[k] = 'reject ' + (e && e.name);
          }
        )
    );
  }
  var names = [
    'fetch',
    'Headers',
    'Request',
    'Response',
    'FormData',
    'Blob',
    'File',
    'FileReader',
    'URL',
    'URLSearchParams',
    'TextEncoder',
    'TextDecoder',
    'AbortController',
    'AbortSignal',
    'Event',
    'CustomEvent',
    'EventTarget',
    'MessageEvent',
    'ErrorEvent',
    'MessageChannel',
    'MessagePort',
    'BroadcastChannel',
    'WebSocket',
    'XMLHttpRequest',
    'EventSource',
    'ReadableStream',
    'WritableStream',
    'TransformStream',
    'CompressionStream',
    'DecompressionStream',
    'TextDecoderStream',
    'structuredClone',
    'atob',
    'btoa',
    'crypto',
    'performance',
    'queueMicrotask',
    'setTimeout',
    'setInterval',
    'clearTimeout',
    'indexedDB',
    'caches',
    'Cache',
    'CacheStorage',
    'navigator',
    'location',
    'Worker',
    'createImageBitmap',
    'WebAssembly',
    'Intl',
    'BigInt',
    'Atomics',
    'SharedArrayBuffer',
    'DOMException',
    'DOMParser',
    'ProgressEvent',
    'PromiseRejectionEvent',
    'Notification',
    'CustomEvent',
    'reportError',
    'self',
    'globalThis',
    'console',
    'origin',
    'isSecureContext',
    'crossOriginIsolated',
  ];
  o.globals = names
    .filter(function (n) {
      try {
        return typeof globalThis[n] !== 'undefined';
      } catch (e) {
        return false;
      }
    })
    .join();
  t('te', function () {
    return Array.from(new TextEncoder().encode('héllo €'));
  });
  t('te.into', function () {
    var u = new Uint8Array(4);
    var r = new TextEncoder().encodeInto('héllo', u);
    return [r.read, r.written];
  });
  t('te.enc', function () {
    return new TextEncoder().encoding;
  });
  t('td', function () {
    return new TextDecoder().decode(new Uint8Array([104, 195, 169]));
  });
  t('td.bad', function () {
    return new TextDecoder().decode(new Uint8Array([255, 104]));
  });
  t('td.fatal', function () {
    return new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([255]));
  });
  t('td.label', function () {
    return new TextDecoder('UTF8').encoding;
  });
  t('td.latin', function () {
    return new TextDecoder('latin1').decode(new Uint8Array([233]));
  });
  t('td.badlabel', function () {
    return new TextDecoder('nope');
  });
  t('td.bom', function () {
    return new TextDecoder().decode(new Uint8Array([239, 187, 191, 104]));
  });
  t('usp.str', function () {
    return new URLSearchParams({ a: '1 2', b: 'é&=' }).toString();
  });
  t('usp.get', function () {
    var u = new URLSearchParams('a=1&a=2&b=%20x+y');
    return [u.get('a'), u.getAll('a'), u.get('b'), u.has('c'), u.size];
  });
  t('usp.sort', function () {
    var u = new URLSearchParams('b=2&a=1&c=3&a=0');
    u.sort();
    return u.toString();
  });
  t('usp.del', function () {
    var u = new URLSearchParams('a=1&b=2&a=3');
    u.delete('a');
    u.append('z', '9');
    u.set('b', '7');
    return u.toString();
  });
  t('usp.iter', function () {
    return Array.from(new URLSearchParams('a=1&b=2'));
  });
  t('usp.pairs', function () {
    return new URLSearchParams([
      ['x', '1'],
      ['y', '2'],
    ]).toString();
  });
  t('usp.q', function () {
    return new URLSearchParams('?q=1').get('q');
  });
  t('url.parts', function () {
    var u = new URL('https://u:p@ex.com:8080/a/b?x=1#h');
    return [
      u.protocol,
      u.host,
      u.hostname,
      u.port,
      u.pathname,
      u.search,
      u.hash,
      u.origin,
      u.username,
      u.password,
      u.href,
    ];
  });
  t('url.rel', function () {
    return new URL('../c?d#e', 'https://ex.com/a/b/x').href;
  });
  t('url.bad', function () {
    return new URL('nope');
  });
  t('url.dot', function () {
    return new URL('https://ex.com/a/./b/../c').pathname;
  });
  t('url.port', function () {
    return new URL('https://ex.com:443/').port + '|' + new URL('http://ex.com:80/').host;
  });
  t('url.sp', function () {
    var u = new URL('https://ex.com/?a=1');
    u.searchParams.append('b', '2');
    return u.href;
  });
  t('url.set', function () {
    var u = new URL('https://ex.com/');
    u.pathname = '/x y';
    u.search = '?q=é';
    u.hash = 'z';
    return u.href;
  });
  t('url.tostr', function () {
    return String(new URL('https://ex.com')) + '|' + JSON.stringify(new URL('https://ex.com'));
  });
  t('url.can', function () {
    return URL.canParse ? [URL.canParse('https://a'), URL.canParse('x')] : 'none';
  });
  t('url.idn', function () {
    return new URL('https://EX.com/%7Efoo').href;
  });
  t('blob', function () {
    var b = new Blob(['ab', new Uint8Array([99])], { type: 'Text/Plain' });
    return [b.size, b.type, b.slice(1, 3).size, b.slice(-1).size];
  });
  t('blob.slice', function () {
    return new Blob(['abc']).slice(1, 2, 'x/y').type;
  });
  t('file', function () {
    var f = new File(['x'], 'n.txt', { type: 'a/b', lastModified: 5 });
    return [f.name, f.type, f.size, f.lastModified, f instanceof Blob];
  });
  ta('blob.text', function () {
    return new Blob(['hé', 'i']).text();
  });
  ta('blob.ab', function () {
    return new Blob(['ab']).arrayBuffer().then(function (b) {
      return Array.from(new Uint8Array(b));
    });
  });
  ta('blob.bytes', function () {
    return typeof Blob.prototype.bytes;
  });
  ta('blob.stream', function () {
    return typeof Blob.prototype.stream;
  });
  t('fd', function () {
    var f = new FormData();
    f.append('a', '1');
    f.append('a', '2');
    f.set('b', '3');
    f.append('c', new Blob(['x']), 'f.txt');
    var r = [];
    f.forEach(function (v, k) {
      r.push(k + '=' + (typeof v === 'string' ? v : v.name));
    });
    return [f.getAll('a'), f.get('b'), f.has('z'), r];
  });
  t('fd.del', function () {
    var f = new FormData();
    f.append('a', '1');
    f.delete('a');
    return Array.from(f.keys());
  });
  t('ev', function () {
    var e = new Event('x', { bubbles: true, cancelable: true });
    return [e.type, e.bubbles, e.cancelable, e.defaultPrevented, e.target, e.eventPhase];
  });
  t('ev.prevent', function () {
    var et = new EventTarget(),
      r = [];
    et.addEventListener('x', function (e) {
      e.preventDefault();
      r.push('a');
    });
    et.addEventListener('x', function () {
      r.push('b');
    });
    var d = et.dispatchEvent(new Event('x', { cancelable: true }));
    r.push(d);
    return r;
  });
  t('ev.once', function () {
    var et = new EventTarget(),
      n = 0;
    et.addEventListener(
      'x',
      function () {
        n++;
      },
      { once: true }
    );
    et.dispatchEvent(new Event('x'));
    et.dispatchEvent(new Event('x'));
    return n;
  });
  t('ev.remove', function () {
    var et = new EventTarget(),
      n = 0,
      f = function () {
        n++;
      };
    et.addEventListener('x', f);
    et.removeEventListener('x', f);
    et.dispatchEvent(new Event('x'));
    return n;
  });
  t('ev.dup', function () {
    var et = new EventTarget(),
      n = 0,
      f = function () {
        n++;
      };
    et.addEventListener('x', f);
    et.addEventListener('x', f);
    et.dispatchEvent(new Event('x'));
    return n;
  });
  t('ev.obj', function () {
    var et = new EventTarget(),
      n = 0;
    et.addEventListener('x', {
      handleEvent: function () {
        n++;
      },
    });
    et.dispatchEvent(new Event('x'));
    return n;
  });
  t('ev.stop', function () {
    var et = new EventTarget(),
      r = [];
    et.addEventListener('x', function (e) {
      r.push(1);
      e.stopImmediatePropagation();
    });
    et.addEventListener('x', function () {
      r.push(2);
    });
    et.dispatchEvent(new Event('x'));
    return r;
  });
  t('ev.target', function () {
    var et = new EventTarget(),
      r;
    et.addEventListener('x', function (e) {
      r = [e.target === et, e.currentTarget === et, e.eventPhase];
    });
    et.dispatchEvent(new Event('x'));
    return r;
  });
  t('ev.custom', function () {
    var e = new CustomEvent('c', { detail: 5 });
    return [e.type, e.detail];
  });
  t('ev.msg', function () {
    var e = new MessageEvent('m', { data: 1, origin: 'o' });
    return [e.data, e.origin, e.lastEventId, e.ports.length];
  });
  t('ev.noarg', function () {
    return new Event();
  });
  t('ev.ts', function () {
    return typeof new Event('x').timeStamp;
  });
  t('ev.signal', function () {
    var et = new EventTarget(),
      n = 0,
      c = new AbortController();
    et.addEventListener(
      'x',
      function () {
        n++;
      },
      { signal: c.signal }
    );
    et.dispatchEvent(new Event('x'));
    c.abort();
    et.dispatchEvent(new Event('x'));
    return n;
  });
  t('ac', function () {
    var c = new AbortController();
    var r = [c.signal.aborted];
    c.abort();
    r.push(c.signal.aborted, c.signal.reason.name, c.signal.reason.code);
    return r;
  });
  t('ac.reason', function () {
    var c = new AbortController();
    c.abort('why');
    return c.signal.reason;
  });
  t('ac.throw', function () {
    var c = new AbortController();
    c.abort();
    c.signal.throwIfAborted();
  });
  t('ac.event', function () {
    var c = new AbortController(),
      n = 0;
    c.signal.addEventListener('abort', function () {
      n++;
    });
    c.signal.onabort = function () {
      n += 10;
    };
    c.abort();
    c.abort();
    return n;
  });
  t('as.abort', function () {
    var s = AbortSignal.abort();
    return [s.aborted, s.reason.name];
  });
  t('as.timeout', function () {
    return typeof AbortSignal.timeout;
  });
  t('as.any', function () {
    return typeof AbortSignal.any;
  });
  t('as.ctor', function () {
    return new AbortSignal();
  });
  t('as.type', function () {
    return Object.prototype.toString.call(new AbortController().signal);
  });
  t('de', function () {
    var e = new DOMException('m', 'AbortError');
    return [e.name, e.message, e.code, e instanceof Error, String(e)];
  });
  t('de.default', function () {
    var e = new DOMException();
    return [e.name, e.message, e.code];
  });
  t('de.consts', function () {
    return [DOMException.ABORT_ERR, DOMException.NOT_FOUND_ERR];
  });
  t('b64', function () {
    return [btoa('hi'), atob('aGk='), atob(' aG k= '), btoa('')];
  });
  t('b64.bad', function () {
    return atob('***');
  });
  t('b64.wide', function () {
    return btoa('Ā');
  });
  t('sc', function () {
    var m = new Map([[1, { a: [1, 2] }]]),
      c = structuredClone(m);
    return [c instanceof Map, c.get(1).a, c !== m];
  });
  t('sc.cyc', function () {
    var a = {};
    a.a = a;
    var c = structuredClone(a);
    return c.a === c;
  });
  t('sc.fn', function () {
    return structuredClone(function () {});
  });
  t('sc.date', function () {
    return structuredClone(new Date(5)).getTime();
  });
  t('sc.err', function () {
    var e = structuredClone(new RangeError('x'));
    return [e.name, e.message];
  });
  t('sc.set', function () {
    return Array.from(structuredClone(new Set([1, 2])));
  });
  t('sc.re', function () {
    var r = structuredClone(/a/gi);
    return [r.source, r.flags];
  });
  t('sc.ta', function () {
    return Array.from(structuredClone(new Uint8Array([1, 2])));
  });
  t('sc.big', function () {
    return String(structuredClone(5n));
  });
  t('uuid', function () {
    var u = crypto.randomUUID();
    return [u.length, /^[0-9a-f-]{36}$/.test(u), u[14]];
  });
  t('rv', function () {
    var a = new Uint8Array(8);
    crypto.getRandomValues(a);
    return a.length;
  });
  t('rv.bad', function () {
    return crypto.getRandomValues(new Float32Array(2));
  });
  t('subtle', function () {
    return typeof crypto.subtle;
  });
  t('perf', function () {
    return [
      typeof performance.now(),
      typeof performance.timeOrigin,
      typeof performance.mark,
      typeof performance.measure,
      typeof performance.getEntriesByName,
    ];
  });
  t('perf.mark', function () {
    performance.mark('p');
    return performance.getEntriesByName('p').length;
  });
  t('qm', function () {
    return typeof queueMicrotask;
  });
  t('sort', function () {
    var r = [];
    setTimeout(function () {
      r.push('t');
    }, 0);
    Promise.resolve().then(function () {
      r.push('p');
    });
    queueMicrotask(function () {
      r.push('q');
    });
    r.push('s');
    o.order = r;
    return 'x';
  });
  t('response', function () {
    var r = new Response('x', { status: 201, statusText: 'C' });
    return [r.status, r.ok, r.statusText, r.type, r.bodyUsed, r.url, r.redirected];
  });
  t('response.json', function () {
    return Response.json({ a: 1 }).headers.get('content-type');
  });
  t('response.err', function () {
    return [Response.error().type, Response.error().status];
  });
  t('response.redirect', function () {
    var r = Response.redirect('https://a/b', 301);
    return [r.status, r.headers.get('location')];
  });
  t('response.null', function () {
    var r = new Response(null);
    return [r.status, r.body];
  });
  t('response.status', function () {
    return new Response('x', { status: 99 });
  });
  ta('response.text', function () {
    return new Response('hé').text();
  });
  ta('response.ab', function () {
    return new Response('ab').arrayBuffer().then(function (b) {
      return b.byteLength;
    });
  });
  ta('response.blob', function () {
    return new Response('ab', { headers: { 'content-type': 'x/y' } }).blob().then(function (b) {
      return [b.size, b.type];
    });
  });
  ta('response.json2', function () {
    return new Response('{"a":[1]}').json();
  });
  ta('response.used', function () {
    var r = new Response('x');
    return r
      .text()
      .then(function () {
        return [
          r.bodyUsed,
          r.text().then(
            function () {
              return 'no';
            },
            function (e) {
              return e.name;
            }
          ),
        ];
      })
      .then(function (v) {
        return Promise.resolve(v[1]).then(function (x) {
          return [v[0], x];
        });
      });
  });
  ta('response.clone', function () {
    var r = new Response('abc'),
      c = r.clone();
    return Promise.all([r.text(), c.text()]);
  });
  ta('response.form', function () {
    return new Response('a=1&b=2', { headers: { 'content-type': 'application/x-www-form-urlencoded' } })
      .formData()
      .then(function (f) {
        return [f.get('a'), f.get('b')];
      });
  });
  ta('response.body', function () {
    return typeof new Response('x').body;
  });
  ta('response.stream', function () {
    return new Response('hello').body
      .getReader()
      .read()
      .then(function (r) {
        return [r.done, r.value.length];
      });
  });
  t('request', function () {
    var r = new Request('https://a/b', { method: 'post', body: 'x' });
    return [r.method, r.url, r.mode, r.credentials, r.cache, r.redirect, r.referrer, r.bodyUsed];
  });
  t('request.get', function () {
    return new Request('https://a/b', { method: 'GET', body: 'x' });
  });
  t('request.clone', function () {
    var r = new Request('https://a/b', { headers: { x: '1' } }).clone();
    return r.headers.get('x');
  });
  t('request.rel', function () {
    return new Request('/z').url;
  });
  t('request.signal', function () {
    return typeof new Request('https://a/').signal;
  });
  ta('request.text', function () {
    return new Request('https://a/', { method: 'POST', body: 'hey' }).text();
  });
  ta('request.json', function () {
    return new Request('https://a/', { method: 'POST', body: '{"a":1}' }).json();
  });
  ta('request.ab', function () {
    return new Request('https://a/', { method: 'POST', body: 'hey' }).arrayBuffer().then(function (b) {
      return b.byteLength;
    });
  });
  t('request.ctype', function () {
    return new Request('https://a/', { method: 'POST', body: new URLSearchParams({ a: '1' }) }).headers.get(
      'content-type'
    );
  });
  t('request.blobtype', function () {
    return new Request('https://a/', { method: 'POST', body: new Blob(['x'], { type: 'q/r' }) }).headers.get(
      'content-type'
    );
  });
  t('headers', function () {
    var h = new Headers({ B: '2', a: '1' });
    h.append('b', '3');
    return [Array.from(h), h.get('A'), h.has('b'), Array.from(h.keys())];
  });
  t('headers.bad', function () {
    return new Headers([['a']]);
  });
  t('headers.del', function () {
    var h = new Headers({ a: '1' });
    h.delete('a');
    return Array.from(h);
  });
  t('headers.cookie', function () {
    var h = new Headers();
    h.append('Set-Cookie', 'a=1');
    h.append('Set-Cookie', 'b=2');
    return typeof h.getSetCookie === 'function' ? h.getSetCookie() : 'none';
  });
  t('headers.trim', function () {
    return new Headers({ a: '  x  ' }).get('a');
  });
  t('headers.tostr', function () {
    return Object.prototype.toString.call(new Headers());
  });
  ta('fr', function () {
    return new Promise(function (res) {
      var r = new FileReader(),
        ev = [];
      r.onloadstart = function () {
        ev.push('ls');
      };
      r.onload = function () {
        ev.push('l');
      };
      r.onloadend = function () {
        ev.push('le');
        res([ev, r.result, r.readyState]);
      };
      r.readAsText(new Blob(['hi']));
    });
  });
  ta('fr.dataurl', function () {
    return new Promise(function (res) {
      var r = new FileReader();
      r.onload = function () {
        res(r.result);
      };
      r.readAsDataURL(new Blob(['hi'], { type: 'a/b' }));
    });
  });
  ta('fr.ab', function () {
    return new Promise(function (res) {
      var r = new FileReader();
      r.onload = function () {
        res(r.result.byteLength);
      };
      r.readAsArrayBuffer(new Blob(['hi']));
    });
  });
  t('ts.cs', function () {
    return typeof CompressionStream;
  });
  t('timers', function () {
    var id = setTimeout(function () {}, 1000);
    clearTimeout(id);
    return typeof id;
  });
  t('mc', function () {
    var m = new MessageChannel();
    return [typeof m.port1.postMessage, typeof m.port2.start, typeof m.port1.close];
  });
  t('ws', function () {
    return [WebSocket.CONNECTING, WebSocket.OPEN, WebSocket.CLOSING, WebSocket.CLOSED];
  });
  t('xhr', function () {
    var x = new XMLHttpRequest();
    return [x.readyState, XMLHttpRequest.DONE, x.status, x.responseType, x.withCredentials, typeof x.upload];
  });
  t('xhr.state', function () {
    var x = new XMLHttpRequest();
    x.open('GET', 'https://a/');
    return x.readyState;
  });
  t('nav', function () {
    return [
      typeof navigator.userAgent,
      typeof navigator.language,
      typeof navigator.languages,
      typeof navigator.hardwareConcurrency,
      typeof navigator.onLine,
    ];
  });
  t('err.stack', function () {
    return typeof new Error('x').stack;
  });
  t('json', function () {
    return JSON.stringify({ a: [1, { b: undefined, c: null }], d: ' ' });
  });
  t('intl', function () {
    return [typeof Intl.DateTimeFormat, typeof Intl.NumberFormat, new Intl.NumberFormat('en-US').format(1234567.891)];
  });
  t('date', function () {
    return new Date(0).toISOString();
  });
  t('math', function () {
    return [
      Math.hypot(3, 4),
      Math.clz32(1),
      Math.fround(5.5),
      Math.trunc(-4.7),
      Math.sign(-3),
      Math.cbrt(27),
      Math.log2(8),
      Math.expm1(0),
    ];
  });
  t('str', function () {
    return [
      'abc'.padStart(5, '-'),
      'a-b'.replaceAll('-', '+'),
      'x'.at(-1),
      'é'.normalize('NFD').length,
      'ab'.localeCompare('ac'),
    ];
  });
  t('arr', function () {
    return [
      [3, 1, 2].toSorted ? [3, 1, 2].toSorted() : 'none',
      [1, 2, 3].at(-1),
      [1, [2, [3]]].flat(Infinity),
      Object.fromEntries([['a', 1]]),
      [1, 2, 3].findLast(function (x) {
        return x < 3;
      }),
    ];
  });
  t('sym', function () {
    return [
      typeof Symbol.asyncIterator,
      typeof Symbol.dispose,
      typeof WeakRef,
      typeof FinalizationRegistry,
      typeof globalThis.gc,
    ];
  });
  t('promise', function () {
    return [typeof Promise.allSettled, typeof Promise.any, typeof Promise.withResolvers, typeof Array.fromAsync];
  });
  t('ta', function () {
    var a = new Uint8Array([1, 2, 3]);
    return [
      a.toReversed ? 'y' : 'n',
      a.subarray(1).length,
      Array.from(new DataView(a.buffer).getUint16(0, true).toString()),
    ];
  });
  t('ab', function () {
    var b = new ArrayBuffer(8, { maxByteLength: 16 });
    return [b.resizable, b.slice(2).byteLength, ArrayBuffer.isView(b), typeof b.transfer];
  });
  return Promise.all(jobs).then(function () {
    return o;
  });
}
