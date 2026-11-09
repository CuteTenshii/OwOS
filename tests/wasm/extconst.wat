(module
  (import "env" "a" (global $a i32))
  (global (export "b") i32 (i32.add (global.get $a) (i32.mul (i32.const 3) (i32.const 4))))
  (global (export "c") i64 (i64.sub (i64.const 100) (i64.mul (i64.const 6) (i64.const 7))))
  (memory 1)
  (data (i32.add (global.get $a) (i32.const 6)) "hi")
  (func (export "rd") (result i32) i32.const 16 i32.load8_u))
