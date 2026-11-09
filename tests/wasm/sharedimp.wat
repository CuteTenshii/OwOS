(module
  (import "env" "m" (memory 1 2 shared))
  (func (export "ld") (param i32) (result i32) local.get 0 i32.atomic.load)
  (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.atomic.rmw.add)
  (func (export "size") (result i32) memory.size))
