set pagination off
break types.gopp:33
break types.gopp:46
run
echo @@SCALARS\n
info args
info locals
bt
continue
echo @@AGGREGATES\n
info args
info locals
echo @@PRINT\n
p d
p d.tags
p d.pos
p arr
p arr[1]
p *p
p o
p t
p h
p m
p s
p xs
whatis o
whatis xs
whatis s
whatis t
ptype h
echo @@GLOBALS\n
p 'main.gcount'
p 'main.gname'
p 'main.gvec'
echo @@BACKTRACE\n
bt
up
echo @@CALLER\n
info locals
