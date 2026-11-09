(module
  (table $t (export "t") 4 externref)
  (global $g (export "g") (mut externref) (ref.null extern))
  (func (export "id") (param externref) (result externref) local.get 0)
  (func (export "isnull") (param externref) (result i32) local.get 0 ref.is_null)
  (func (export "tset") (param i32 externref) local.get 0 local.get 1 table.set $t)
  (func (export "tget") (param i32) (result externref) local.get 0 table.get $t)
  (func (export "gset") (param externref) local.get 0 global.set $g)
  (func (export "gget") (result externref) global.get $g)
  (func (export "grow") (param externref i32) (result i32) local.get 0 local.get 1 table.grow $t)
  (func (export "fill") (param i32 externref i32) local.get 0 local.get 1 local.get 2 table.fill $t)
  (func (export "pass3") (param externref externref externref) (result externref externref externref) local.get 2 local.get 1 local.get 0)
  (func (export "loop") (param externref i32) (result externref) (local i32)
    block loop local.get 2 local.get 1 i32.ge_u br_if 1
      i32.const 0 local.get 0 table.set $t i32.const 0 table.get $t local.set 0
      local.get 2 i32.const 1 i32.add local.set 2 br 0 end end local.get 0)
)
