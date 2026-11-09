(module
  (import "env" "mem" (memory 1 1 shared))
  (func (export "inc") i32.const 0 i32.const 1 i32.atomic.rmw.add drop)
  (func (export "wait") (result i32) i32.const 4 i32.const 0 i64.const -1 memory.atomic.wait32)
  (func (export "signal")
    i32.const 4 i32.const 1 i32.atomic.store
    i32.const 4 i32.const 100 memory.atomic.notify drop))
