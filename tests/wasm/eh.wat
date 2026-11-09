(module
  (import "env" "jsthrow" (func $jsthrow (param i32)))
  (import "env" "jt" (tag $jt (param i32)))
  (tag $e0 (param i32))
  (tag $e1)
  (tag $e2 (param i64 f64))
  (export "e0" (tag $e0))
  (export "e1" (tag $e1))
  (func $thrower (param i32) local.get 0 throw $e0)
  (func (export "catch0") (param i32) (result i32)
    try (result i32)
      local.get 0 call $thrower i32.const -1
    catch $e0 i32.const 100 i32.add
    end)
  (func (export "catchall") (param i32) (result i32)
    try (result i32)
      local.get 0 if throw $e1 end i32.const 1
    catch_all i32.const 2
    end)
  (func (export "nomatch") (param i32)
    try local.get 0 call $thrower catch $e1 end)
  (func (export "rethrow") (param i32) (result i32)
    try (result i32)
      try (result i32) local.get 0 call $thrower i32.const 0
      catch $e0 drop rethrow 0
      end
    catch $e0 i32.const 1000 i32.add
    end)
  (func (export "delegate") (param i32) (result i32)
    try (result i32)
      try (result i32) local.get 0 call $thrower i32.const 0 delegate 0
      catch $e0 i32.const 7 i32.add
    end)
  (func (export "delegate_out") (param i32) (result i32)
    try (result i32)
      try (result i32) local.get 0 call $thrower i32.const 0 delegate 0
    catch $e1 i32.const 7
    end)
  (func (export "pair") (result i64 f64)
    try (result i64 f64) i64.const 5 f64.const 1.5 throw $e2 catch $e2 end)
  (func (export "js") (param i32) (result i32)
    try (result i32) local.get 0 call $jsthrow i32.const 0 catch_all i32.const 99 end)
  (func (export "jstag") (param i32) (result i32)
    try (result i32) local.get 0 call $jsthrow i32.const 0 catch $jt i32.const 10 i32.add end)
  (func (export "jsre") (param i32) (result i32)
    try (result i32) local.get 0 call $jsthrow i32.const 0 catch_all rethrow 0 end)
  (func (export "trap") (result i32) try (result i32) unreachable catch_all i32.const 1 end)
  (func (export "under") (param i32) (result i32)
    i32.const 5 i32.const 6
    try (result i32) local.get 0 call $thrower i32.const 0 catch $e0 i32.const 1 i32.add end
    i32.add i32.add)
  (func (export "loopc") (param i32) (result i32) (local i32)
    block loop
      local.get 0 i32.eqz br_if 1
      try local.get 0 call $thrower catch $e0 local.get 1 i32.add local.set 1 end
      local.get 0 i32.const 1 i32.sub local.set 0
      br 0
    end end local.get 1)
  (func (export "brout") (param i32) (result i32)
    block (result i32)
      try (result i32) local.get 0 call $thrower i32.const 0 catch $e0 i32.const 3 i32.add br 1 end
    end i32.const 1 i32.add)
  (func (export "deep") (param i32) (result i32)
    try (result i32) local.get 0 call $d catch $e0 end)
  (func $d (param i32) (result i32)
    local.get 0 if (result i32) local.get 0 i32.const 1 i32.sub call $d else i32.const 42 call $thrower i32.const 0 end))
