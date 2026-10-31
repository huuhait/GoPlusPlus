set pagination off
break scopes.gopp:10
run
echo @@INNER\n
p x
p y
info args
up
echo @@MAIN\n
p x
