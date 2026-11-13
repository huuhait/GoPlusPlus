// exit: 2
package main

type MyErr struct{ code int }

func (e MyErr) Error() string { return "my error" }

func safe(f func()) (ok bool) {
	defer func() {
		if r := recover(); r != nil {
			println("recovered")
			ok = false
		}
	}()
	f()
	return true
}

func value() (n int) {
	defer func() {
		r := recover()
		if s, is := r.(string); is {
			println("value:", s)
		}
		n = n + 100
	}()
	n = 5
	panic("text")
}

func unnamed() int {
	defer func() {
		recover()
	}()
	panic(1)
}

func deep(n int) {
	if n == 0 {
		panic(MyErr{7})
	}
	defer println("leaving", n)
	deep(n - 1)
}

func index(s []int, i int) (v int) {
	defer func() {
		if recover() != nil {
			v = -1
		}
	}()
	return s[i]
}

func nothing() {
	defer func() {
		if recover() == nil {
			println("no panic")
		}
	}()
}

func main() {
	println(safe(func() {}))
	println(safe(func() { panic("boom") }))
	println(value())
	println(unnamed())
	println(safe(func() { deep(3) }))
	println(index([]int{1, 2, 3}, 1), index([]int{1, 2, 3}, 5))
	nothing()
	var e error = MyErr{1}
	println(safe(func() { panic(e) }))
	defer println("deferred in main")
	panic(MyErr{2})
}
