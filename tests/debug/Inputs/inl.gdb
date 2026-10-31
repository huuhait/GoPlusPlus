set pagination off
break b.gopp:20
break b.gopp:21
run
echo @@STOP1\n
bt
info frame
info args
info locals
frame 1
echo @@CALLER1\n
info args
frame 2
echo @@CALLER2\n
info locals
continue
echo @@STOP2\n
info locals
continue
echo @@STOP3\n
bt
info locals
