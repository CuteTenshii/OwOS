(module
  (memory (export "mem") 2)
  (table (export "tab") 1 funcref)
  (global (export "g") i32 (i32.const 3))
  (func (export "f") (param i32 i32) (result i32) i32.const 0)
  (func (export "dup") (result i32) i32.const 1)
  (export "alias" (func 1))
  (@custom "mysec" "payload"))
