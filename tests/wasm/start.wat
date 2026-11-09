(module
  (import "env" "note" (func $note (param i32)))
  (memory (export "m") 1)
  (func $s i32.const 1 call $note i32.const 0 i32.const 77 i32.store i32.const 2 call $note)
  (start $s)
  (func (export "get") (result i32) i32.const 0 i32.load))
