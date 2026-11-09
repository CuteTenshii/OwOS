(module
  (tag $e (param i32))
  (tag $f)
  (export "e" (tag $e))
  (func (export "tt") (param i32) (result i32)
    block $h (result i32)
      try_table (result i32) (catch $e $h)
        local.get 0 throw $e
      end
    end)
  (func (export "tt_all") (param i32) (result i32)
    block $h
      try_table (catch_all $h)
        local.get 0 if throw $f end
      end
      i32.const 1 return
    end
    i32.const 2)
  (func (export "tt_ref") (param i32) (result i32)
    (local $x exnref)
    block $outer (result i32)
      try_table (result i32) (catch $e $outer)
        block $h (result i32 exnref)
          try_table (result i32 exnref) (catch_ref $e $h)
            local.get 0 throw $e
          end
        end
        local.set $x
        drop
        local.get $x throw_ref
      end
    end)
  (func (export "mk") (param i32) (result exnref)
    block $h (result exnref)
      try_table (catch_all_ref $h)
        local.get 0 throw $e
      end
      ref.null exn
    end)
  (func (export "thr") (param exnref) local.get 0 throw_ref)
  (func (export "isnull") (param exnref) (result i32) local.get 0 ref.is_null)
  (func (export "nomatch") (param i32)
    block $h (result i32)
      try_table (result i32) (catch $e $h) throw $f end
    end drop)
  (func (export "nested") (param i32) (result i32)
    block $a (result i32)
      try_table (result i32) (catch $e $a)
        block $b
          try_table (catch $f $b) local.get 0 throw $e end
        end
        i32.const 5
      end
    end i32.const 100 i32.add))
