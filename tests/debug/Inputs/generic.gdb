set pagination off
set confirm off
break main.show
run
echo @@WHATIS\n
whatis p
whatis b
whatis q
echo @@PRINT\n
print p
print b
print q.Key
echo @@PLAIN\n
whatis b.V
bt 1
