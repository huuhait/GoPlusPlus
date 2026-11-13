package main

type Counter struct {
	inc func() int
	get func() int
}

func counter(start int) Counter {
	n := start
	return Counter{
		inc: func() int { n++; return n },
		get: func() int { return n },
	}
}

func adder(by int) func(int) int {
	return func(x int) int { return x + by }
}

func compose(f, g func(int) int) func(int) int {
	return func(x int) int { return g(f(x)) }
}

func apply(values []int, f func(int)) {
	for _, v := range values {
		f(v)
	}
}

func named() (total int, text string) {
	add := func(n int, s string) {
		total += n
		text += s
	}
	add(1, "a")
	add(2, "b")
	return
}

func join(parts []string) string {
	out := ""
	sep := ""
	each := func(s string) {
		out += sep + s
		sep = ","
	}
	for _, p := range parts {
		each(p)
	}
	return out
}

func nested(a int) func() func() int {
	b := a * 2
	return func() func() int {
		c := b + 1
		return func() int {
			a++
			return a + b + c
		}
	}
}

// A result that a literal uses, in a function with a deferred call.
func deferred(start int) (n int, note string) {
	defer println("left", start)
	bump := func(by int) {
		n += by
		note += "+"
	}
	bump(start)
	bump(1)
	return n + 1, note + "!"
}

func main() {
	n, note := deferred(4)
	println(n, note)
	c := counter(10)
	c.inc()
	c.inc()
	println(c.get(), c.inc(), c.get())
	d := counter(0)
	println(d.get(), c.get())

	add2 := adder(2)
	times := func(x int) int { return x * 3 }
	both := compose(add2, times)
	println(add2(1), both(1), compose(times, add2)(1))

	sum := 0
	apply([]int{1, 2, 3}, func(v int) { sum += v })
	println(sum)
	total, text := named()
	println(total, text, join([]string{"x", "y", "z"}))

	// Each pass of a loop has its own variable.
	var fs []func() int
	for i := 0; i < 3; i++ {
		fs = append(fs, func() int { return i * 10 })
	}
	for _, v := range []int{7, 8} {
		fs = append(fs, func() int { return v })
	}
	for i := range 2 {
		j := i + 100
		fs = append(fs, func() int { j++; return j })
	}
	for _, f := range fs {
		println(f())
	}
	println(fs[5](), fs[5](), fs[6]())

	deep := nested(1)
	inner := deep()
	println(inner(), inner(), deep()())

	words := []string{"a", "b"}
	grow := func(s string) int {
		words = append(words, s)
		return len(words)
	}
	println(grow("c"), grow("d"), len(words), words[3])

	x := 1
	func() {
		x += 10
		func() { x *= 2 }()
	}()
	println(x)
	param := func(p int) func() int {
		return func() int { p++; return p }
	}(5)
	println(param(), param())
}
