/* The script half of FileReader, ProgressEvent and the DataTransfer family, stringified by the preprocessor (see
 * filedata.c): the natives come in as N. Keep it free of single quotes, backslashes and double-slash comments, and
 * end every statement with a semicolon: the line breaks are lost. This file is not run through clang-format. */
JS(
(function (N) {
  var S = Symbol("filedata");
  function hide(o, k, v) { Object.defineProperty(o, k, { value: v, writable: true, enumerable: false, configurable: true }); }
  function accessor(o, k, get, set) { Object.defineProperty(o, k, { get: get, set: set, enumerable: true, configurable: true }); }
  function illegal() { throw new TypeError("Illegal constructor"); }
  function inherit(C, P) {
    C.prototype = Object.create(P.prototype);
    hide(C.prototype, "constructor", C);
  }

  function ProgressEvent(type, init) {
    var e = new Event(type, init);
    Object.setPrototypeOf(e, ProgressEvent.prototype);
    e.lengthComputable = !!(init && init.lengthComputable);
    e.loaded = init && init.loaded !== undefined ? Number(init.loaded) : 0;
    e.total = init && init.total !== undefined ? Number(init.total) : 0;
    return e;
  }
  inherit(ProgressEvent, Event);

  var HANDLERS = ["onloadstart", "onprogress", "onload", "onabort", "onerror", "onloadend"];
  function FileReader() {
    EventTarget.call(this);
    hide(this, S, { state: 0, token: 0, size: 0, result: null, error: null });
    for (var i = 0; i < HANDLERS.length; i++) { this[HANDLERS[i]] = null; }
  }
  inherit(FileReader, EventTarget);
  [["EMPTY", 0], ["LOADING", 1], ["DONE", 2]].forEach(function (p) {
    Object.defineProperty(FileReader, p[0], { value: p[1], enumerable: true });
    Object.defineProperty(FileReader.prototype, p[0], { value: p[1], enumerable: true });
  });
  accessor(FileReader.prototype, "readyState", function () { return this[S].state; });
  accessor(FileReader.prototype, "result", function () { return this[S].result; });
  accessor(FileReader.prototype, "error", function () { return this[S].error; });
  function fire(r, type, loaded) {
    var total = r[S].size;
    r.dispatchEvent(new ProgressEvent(type, { lengthComputable: true, loaded: loaded, total: total }));
  }
  function read(r, blob, mode) {
    if (!(blob instanceof Blob)) { throw new TypeError("Failed to execute read on FileReader: parameter 1 is not of type Blob."); }
    var st = r[S];
    if (st.state === 1) { throw new DOMException("The object is already busy reading Blobs.", "InvalidStateError"); }
    var token = ++st.token;
    st.state = 1;
    st.result = null;
    st.error = null;
    st.size = blob.size;
    fire(r, "loadstart", 0);
    setTimeout(function () {
      if (st.token !== token) { return; }
      st.result = N.read(blob, mode);
      st.state = 2;
      fire(r, "progress", st.size);
      fire(r, "load", st.size);
      fire(r, "loadend", st.size);
    }, 0);
  }
  hide(FileReader.prototype, "readAsText", function readAsText(blob) { read(this, blob, 0); });
  hide(FileReader.prototype, "readAsArrayBuffer", function readAsArrayBuffer(blob) { read(this, blob, 1); });
  hide(FileReader.prototype, "readAsBinaryString", function readAsBinaryString(blob) { read(this, blob, 2); });
  hide(FileReader.prototype, "readAsDataURL", function readAsDataURL(blob) { read(this, blob, 3); });
  hide(FileReader.prototype, "abort", function abort() {
    var st = this[S];
    if (st.state !== 1) { return; }
    st.token++;
    st.state = 2;
    st.result = null;
    fire(this, "abort", 0);
    fire(this, "loadend", 0);
  });

  function normalize(format) {
    var f = String(format).toLowerCase();
    return f === "text" ? "text/plain" : f === "url" ? "text/uri-list" : f;
  }
  function find(st, type) {
    for (var i = 0; i < st.list.length; i++) {
      if (st.list[i].kind === "string" && st.list[i].type === type) { return st.list[i]; }
    }
    return null;
  }
  function itemOf(st, entry) {
    if (!entry.item) {
      entry.item = Object.create(DataTransferItem.prototype);
      hide(entry.item, S, entry);
    }
    return entry.item;
  }
  function changed(st) {
    var items = st.items, i = 0;
    st.types = null;
    st.files = null;
    for (; i < st.list.length; i++) { items[i] = itemOf(st, st.list[i]); }
    for (; i < st.shown; i++) { delete items[i]; }
    st.shown = st.list.length;
  }
  function drop(st, keep) {
    var kept = [];
    for (var i = 0; i < st.list.length; i++) {
      if (keep(st.list[i])) { kept.push(st.list[i]); } else { st.list[i].gone = true; }
    }
    st.list = kept;
    changed(st);
  }

  function DataTransferItem() { illegal(); }
  accessor(DataTransferItem.prototype, "kind", function () { return this[S].gone ? "" : this[S].kind; });
  accessor(DataTransferItem.prototype, "type", function () { return this[S].gone ? "" : this[S].type; });
  hide(DataTransferItem.prototype, "getAsString", function getAsString(callback) {
    var e = this[S];
    if (typeof callback !== "function" || e.gone || e.kind !== "string") { return; }
    setTimeout(function () { callback(e.data); }, 0);
  });
  hide(DataTransferItem.prototype, "getAsFile", function getAsFile() {
    var e = this[S];
    return e.gone || e.kind !== "file" ? null : e.data;
  });
  hide(DataTransferItem.prototype, "webkitGetAsEntry", function webkitGetAsEntry() { return null; });

  function DataTransferItemList() { illegal(); }
  accessor(DataTransferItemList.prototype, "length", function () { return this[S].list.length; });
  hide(DataTransferItemList.prototype, "add", function add(data, type) {
    var st = this[S], entry;
    if (data instanceof File) {
      entry = { kind: "file", type: data.type, data: data };
    } else {
      if (arguments.length < 2) { throw new TypeError("Failed to execute add on DataTransferItemList: 2 arguments required for a string."); }
      var t = String(type).toLowerCase();
      if (find(st, t)) { throw new DOMException("An item already exists for type " + t + ".", "NotSupportedError"); }
      entry = { kind: "string", type: t, data: String(data) };
    }
    st.list.push(entry);
    changed(st);
    return itemOf(st, entry);
  });
  hide(DataTransferItemList.prototype, "remove", function remove(index) {
    var st = this[S], i = Number(index) >>> 0;
    if (i >= st.list.length) { return; }
    st.list[i].gone = true;
    st.list.splice(i, 1);
    changed(st);
  });
  hide(DataTransferItemList.prototype, "clear", function clear() { drop(this[S], function () { return false; }); });
  hide(DataTransferItemList.prototype, Symbol.iterator, function () { return Array.prototype.slice.call(this)[Symbol.iterator](); });

  var EFFECTS = ["none", "copy", "copyLink", "copyMove", "link", "linkMove", "move", "all", "uninitialized"];
  var DROPS = ["none", "copy", "link", "move"];
  function DataTransfer() {
    var st = { list: [], shown: 0, types: null, files: null, effect: "none", drop: "none", items: null };
    hide(this, S, st);
    st.items = Object.create(DataTransferItemList.prototype);
    hide(st.items, S, st);
  }
  accessor(DataTransfer.prototype, "dropEffect", function () { return this[S].drop; }, function (v) {
    if (DROPS.indexOf(String(v)) >= 0) { this[S].drop = String(v); }
  });
  accessor(DataTransfer.prototype, "effectAllowed", function () { return this[S].effect; }, function (v) {
    if (EFFECTS.indexOf(String(v)) >= 0) { this[S].effect = String(v); }
  });
  accessor(DataTransfer.prototype, "items", function () { return this[S].items; });
  accessor(DataTransfer.prototype, "types", function () {
    var st = this[S];
    if (!st.types) {
      var types = [], files = false;
      for (var i = 0; i < st.list.length; i++) {
        if (st.list[i].kind === "string") { types.push(st.list[i].type); } else { files = true; }
      }
      if (files) { types.push("Files"); }
      st.types = Object.freeze(types);
    }
    return st.types;
  });
  accessor(DataTransfer.prototype, "files", function () {
    var st = this[S];
    if (!st.files) {
      var files = [];
      for (var i = 0; i < st.list.length; i++) {
        if (st.list[i].kind === "file") { files.push(st.list[i].data); }
      }
      st.files = N.fileList(files);
    }
    return st.files;
  });
  hide(DataTransfer.prototype, "getData", function getData(format) {
    if (arguments.length < 1) { throw new TypeError("Failed to execute getData on DataTransfer: 1 argument required."); }
    var st = this[S], url = String(format).toLowerCase() === "url", entry = find(st, normalize(format));
    if (!entry) { return ""; }
    if (!url) { return entry.data; }
    var lines = entry.data.split(String.fromCharCode(10));
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i];
      if (line.charCodeAt(line.length - 1) === 13) { line = line.slice(0, -1); }
      if (line.length > 0 && line.charAt(0) !== "#") { return line; }
    }
    return "";
  });
  hide(DataTransfer.prototype, "setData", function setData(format, data) {
    if (arguments.length < 2) { throw new TypeError("Failed to execute setData on DataTransfer: 2 arguments required."); }
    var st = this[S], type = normalize(format), entry = find(st, type);
    if (entry) {
      entry.data = String(data);
    } else {
      st.list.push({ kind: "string", type: type, data: String(data) });
      changed(st);
    }
  });
  hide(DataTransfer.prototype, "clearData", function clearData(format) {
    var type = format === undefined ? null : normalize(format);
    drop(this[S], function (e) { return e.kind === "file" || (type !== null && e.type !== type); });
  });
  hide(DataTransfer.prototype, "setDragImage", function setDragImage(image, x, y) {
    if (arguments.length < 3) { throw new TypeError("Failed to execute setDragImage on DataTransfer: 3 arguments required."); }
  });

  return { FileReader: FileReader, ProgressEvent: ProgressEvent, DataTransfer: DataTransfer, DataTransferItem: DataTransferItem, DataTransferItemList: DataTransferItemList };
})
)
