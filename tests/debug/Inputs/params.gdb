set pagination off
break params.gopp:12
break params.gopp:19
break params.gopp:30
run
echo @@GROW\n
info args
print sizeof(xs)
continue
echo @@MAIN\n
print m->count
print m->alloc
print sizeof(a)
continue
echo @@SUM\n
info args
echo @@LOCALS\n
info locals
echo @@END\n
print sizeof(xs)
bt 1
