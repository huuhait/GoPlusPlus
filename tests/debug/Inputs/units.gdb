set pagination off
set confirm off
break main.show
run
echo @@UNITS\n
whatis d
print d
whatis n
print n
whatis p
print p
next
echo @@LOCAL\n
whatis speed
print speed
up
echo @@PLAIN\n
whatis plain
print plain
