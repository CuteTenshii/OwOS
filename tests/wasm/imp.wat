(module
  (import "env" "log" (func $log (param i32)))
  (import "env" "add" (func $add (param i32 i32) (result i32)))
  (import "env" "big" (func $big (param i64) (result i64)))
  (import "env" "fl" (func $fl (param f32 f64) (result f64)))
  (import "env" "pair" (func $pair (param i32) (result i32 i64)))
  (import "env" "boom" (func $boom))
  (import "env" "cb" (func $cb (param i32) (result i32)))
  (func (export "t_log") (param i32) local.get 0 call $log)
  (func (export "t_add") (param i32 i32) (result i32) local.get 0 local.get 1 call $add)
  (func (export "t_big") (param i64) (result i64) local.get 0 call $big)
  (func (export "t_fl") (param f32 f64) (result f64) local.get 0 local.get 1 call $fl)
  (func (export "t_pair") (param i32) (result i32 i64) local.get 0 call $pair)
  (func (export "t_boom") call $boom)
  (func (export "t_cb") (param i32) (result i32) local.get 0 call $cb)
  (func (export "dbl") (param i32) (result i32) local.get 0 i32.const 2 i32.mul)
  (func $loopcb (export "loopcb") (param i32) (result i32) (local i32)
    block loop
      local.get 0 i32.eqz br_if 1
      local.get 1 local.get 0 call $cb i32.add local.set 1
      local.get 0 i32.const 1 i32.sub local.set 0 br 0
    end end local.get 1)
  (func (export "log_many") (param i32) (local i32)
    block loop local.get 1 local.get 0 i32.ge_u br_if 1
      local.get 1 call $log local.get 1 i32.const 1 i32.add local.set 1 br 0 end end)
)
