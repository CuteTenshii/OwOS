(module
  (memory (export "mem") 1 4)
  (data (i32.const 8) "\01\02\03\04\05\06\07\08\ff\fe")
  (data $p "hello world")
  (func (export "ld") (param i32 i32) (result i64)
    block block block block block block block block block block block block block
    local.get 1 br_table 0 1 2 3 4 5 6 7 8 9 10 11 12 12
    end local.get 0 i64.load8_s return
    end local.get 0 i64.load8_u return
    end local.get 0 i64.load16_s return
    end local.get 0 i64.load16_u return
    end local.get 0 i64.load32_s return
    end local.get 0 i64.load32_u return
    end local.get 0 i64.load return
    end local.get 0 i32.load8_s i64.extend_i32_s return
    end local.get 0 i32.load8_u i64.extend_i32_u return
    end local.get 0 i32.load16_s i64.extend_i32_s return
    end local.get 0 i32.load16_u i64.extend_i32_u return
    end local.get 0 i32.load i64.extend_i32_s return
    end local.get 0 f64.load i64.reinterpret_f64 return)
  (func (export "st64") (param i32 i64) local.get 0 local.get 1 i64.store)
  (func (export "st32") (param i32 i32) local.get 0 local.get 1 i32.store)
  (func (export "st16") (param i32 i32) local.get 0 local.get 1 i32.store16)
  (func (export "st8") (param i32 i32) local.get 0 local.get 1 i32.store8)
  (func (export "stf") (param i32 f32 f64) local.get 0 local.get 1 f32.store offset=100 local.get 0 local.get 2 f64.store offset=200)
  (func (export "ldf") (param i32) (result f64) local.get 0 f32.load offset=100 f64.promote_f32 local.get 0 f64.load offset=200 f64.add)
  (func (export "off") (param i32) (result i32) local.get 0 i32.load offset=4294967295)
  (func (export "size") (result i32) memory.size)
  (func (export "grow") (param i32) (result i32) local.get 0 memory.grow)
  (func (export "fill") (param i32 i32 i32) local.get 0 local.get 1 local.get 2 memory.fill)
  (func (export "copy") (param i32 i32 i32) local.get 0 local.get 1 local.get 2 memory.copy)
  (func (export "init") (param i32 i32 i32) local.get 0 local.get 1 local.get 2 memory.init $p)
  (func (export "drop") data.drop $p)
  (func (export "last") (result i32) i32.const 65532 i32.load)
)
