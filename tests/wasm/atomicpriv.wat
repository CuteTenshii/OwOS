(module
  (memory 1)
  (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.atomic.rmw.add)
  (func (export "ld") (param i32) (result i32) local.get 0 i32.atomic.load)
  (func (export "wait") (result i32) i32.const 0 i32.const 0 i64.const 0 memory.atomic.wait32)
  (func (export "notify") (result i32) i32.const 0 i32.const 1 memory.atomic.notify))
