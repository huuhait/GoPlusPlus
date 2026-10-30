; Input of eh-spike-tools.test: an invoke that is not alone in its block, with
; PHIs in both its normal and its unwind successor.
declare void @g(i64)
declare i32 @__gxx_personality_v0(...)

define i64 @f(i1 %c, i64 %x) personality ptr @__gxx_personality_v0 {
entry:
  br i1 %c, label %call, label %other
other:
  invoke void @g(i64 0) to label %cont unwind label %lpad
call:
  %y = add i64 %x, 1
  invoke void @g(i64 %y) to label %cont unwind label %lpad
cont:
  %r = phi i64 [ 0, %other ], [ %y, %call ]
  ret i64 %r
lpad:
  %v = phi i64 [ 1, %other ], [ %y, %call ]
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}
