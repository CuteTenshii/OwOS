(module
  (type $t (func (param i32 i32) (result i32)))
  (import "env" "h" (func $h (param i32) (result i32)))
  (table funcref (elem $acc2))
  (func $acc (export "acc") (param i32 i32) (result i32)
    local.get 0 i32.eqz if local.get 1 return end
    local.get 0 i32.const 1 i32.sub local.get 1 local.get 0 i32.add return_call $acc)
  (func $acc2 (param i32 i32) (result i32)
    local.get 0 i32.eqz if local.get 1 return end
    local.get 0 i32.const 1 i32.sub local.get 1 local.get 0 i32.add i32.const 0 return_call_indirect (type $t))
  (func (export "ind") (param i32) (result i32) local.get 0 i32.const 0 call $acc2)
  (func (export "th") (param i32) (result i32) local.get 0 return_call $h)
  (func (export "mv") (param i32) (result i32 i32) local.get 0 local.get 0 i32.const 1 i32.add return_call $mv2)
  (func $mv2 (param i32 i32) (result i32 i32) local.get 1 local.get 0))
