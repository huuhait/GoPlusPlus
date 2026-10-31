set pagination off
break scopes.gopp:10
break scopes.gopp:14
break scopes.gopp:16
break scopes.gopp:22
run
echo @@INNER\n
info locals
p x
p y
continue
echo @@LOOP1\n
info locals
p x
p i
continue
echo @@LOOP2\n
info locals
continue
echo @@AFTER\n
info locals
p y
info args
continue
echo @@MAIN\n
info locals
p 'main.x'
