set pagination off
break thunks.gopp:17
break thunks.gopp:48
break gopp.print_i64
run
echo @@BUMP\n
bt 2
continue
echo @@TWICE\n
bt 2
continue
echo @@RUNTIME\n
bt 2
