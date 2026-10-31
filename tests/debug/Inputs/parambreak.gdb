set pagination off
set print frame-arguments all
break main.scalars
break main.words
break main.place
break main.view
break main.grow
break parambreak.gopp:53
break main.named
break main.misc
break main.Nums.Len
break main.Pair.Sum
break main.Pair.Add
break parambreak.gopp:79
break main.tsc
break main.tpair
break main.tplace
break main.tstr
break main.tslice
break main.Nums.First
run
continue
continue
continue
continue
continue
echo @@GROW-BODY\n
info args
next
info args
continue
echo @@NAMED\n
info locals
next
next
echo @@NAMED-NEXT\n
info args
info locals
continue
continue
echo @@THUNK\n
bt 2
up
info args
info locals
continue
continue
echo @@ADD\n
print *p
continue
continue
continue
continue
continue
continue
continue
continue
echo @@FIRST\n
bt 2
up
info args
info locals
continue
