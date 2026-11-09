(module
  (import "env" "m" (memory i64 1 3))
  (func (export "sz") (result i64) memory.size))
