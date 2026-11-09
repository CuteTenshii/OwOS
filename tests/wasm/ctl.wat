(module
  (func $sum (export "sum") (param i32) (result i32) (local i32)
    block $done
      loop $l
        local.get 0 i32.eqz br_if $done
        local.get 1 local.get 0 i32.add local.set 1
        local.get 0 i32.const 1 i32.sub local.set 0
        br $l
      end
    end
    local.get 1)
  (func (export "sw") (param i32) (result i32)
    block $d block $c block $b block $a
      local.get 0 br_table $a $b $c $d
    end i32.const 10 return
    end i32.const 20 return
    end i32.const 30 return
    end i32.const 40)
  (func (export "swap") (param i32 i32) (result i32 i32) local.get 1 local.get 0)
  (func (export "mvblock") (param i32 i32) (result i32)
    local.get 0 local.get 1
    (block (param i32 i32) (result i32) i32.add))
  (func (export "loopparam") (param i32) (result i32)
    local.get 0
    (loop (param i32) (result i32)
      i32.const 1 i32.sub
      local.tee 0 local.get 0 br_if 0
      drop i32.const 99))
  (func (export "brval") (param i32) (result i32)
    (block (result i32)
      i32.const 7 local.get 0 br_if 0
      drop i32.const 8))
  (func (export "ifelse") (param i32) (result i32)
    local.get 0
    (if (result i32) (then i32.const 1) (else i32.const 2)))
  (func (export "ifnoelse") (param i32) (result i32) (local i32)
    local.get 0 (if (then i32.const 5 local.set 1))
    local.get 1)
  (func (export "dead") (result i32)
    block (result i32) i32.const 3 br 0 i32.const 4 i32.add end)
  (func (export "dead2") (param i32) (result i32)
    (block (result i32)
      local.get 0
      (if (then i32.const 1 br 1 unreachable))
      i32.const 2))
  (func (export "sel") (param i32) (result i32) i32.const 11 i32.const 22 local.get 0 select)
  (func (export "sel64") (param i32) (result i64) i64.const 11 i64.const 22 local.get 0 select)
  (func (export "i64mix") (param i64 i32 f64) (result f64)
    local.get 0 f64.convert_i64_s local.get 1 f64.convert_i32_s f64.add local.get 2 f64.add)
  (func $ev (export "even") (param i32) (result i32)
    local.get 0 i32.eqz if (result i32) i32.const 1 else local.get 0 i32.const 1 i32.sub call $od end)
  (func $od (param i32) (result i32)
    local.get 0 i32.eqz if (result i32) i32.const 0 else local.get 0 i32.const 1 i32.sub call $ev end)
  (func $rec (export "rec") (param i32) (result i32) local.get 0 i32.const 1 i32.add call $rec)
  (func (export "unr") unreachable)
  (func (export "retearly") (param i32) (result i32)
    local.get 0 if i32.const 1 return end i32.const 2)
  (func (export "brif_ret") (param i32) (result i32)
    i32.const 5 local.get 0 br_if 0 drop i32.const 6)
  (func (export "v128loop") (param i32) (result i32) (local v128 v128)
    v128.const i32x4 1 2 3 4 local.set 1
    block loop
      local.get 0 i32.eqz br_if 1
      local.get 1 local.get 2 i32x4.add local.set 2
      local.get 0 i32.const 1 i32.sub local.set 0
      br 0
    end end
    local.get 2 i32x4.extract_lane 3 local.get 2 i32x4.extract_lane 0 i32.add)
  (func (export "tee") (param i32) (result i32) local.get 0 local.tee 0 local.get 0 i32.add)
  (func (export "nested") (param i32) (result i32)
    block (result i32)
      block (result i32)
        block (result i32)
          i32.const 1 local.get 0 br_if 2 drop
          i32.const 2
        end
        i32.const 10 i32.add
      end
      i32.const 100 i32.add
    end)
)
