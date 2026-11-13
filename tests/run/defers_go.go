package main

type counter struct {
	name string
	n    int
}

func (c *counter) add(by int) {
	c.n += by
	println("add", c.name, by, c.n)
}

func (c counter) show(tag string) { println("show", tag, c.name, c.n) }

func say(s string, n int) { println("say", s, n) }

func set(p *int, v int) {
	*p = v
	println("set", v)
}

func result(n int) int {
	println("result", n)
	return n
}

// The calls run when the function is left, the last first, with what
// their arguments were at the defer statement.
func order() {
	n := 1
	defer say("first", n)
	n = 2
	defer say("second", n)
	s := "a"
	defer say(s, 3)
	s = "b"
	println("body", s, n)
}

// A defer that did not run calls nothing.
func branches(n int) int {
	defer say("always", n)
	if n > 1 {
		defer say("big", n)
		if n > 2 {
			return result(n * 10)
		}
	} else {
		defer say("small", n)
	}
	switch n {
	case 2:
		defer say("two", n)
	}
	return result(n)
}

// A pointer is the variable itself: the call sees what was written
// after the defer statement, and its own writes are seen after it.
func pointers() int {
	c := counter{name: "c"}
	defer c.add(100)
	defer c.show("deferred")
	c.add(1)
	total := 5
	defer set(&total, 9)
	c.add(2)
	return total + c.n
}

func nested() {
	defer say("outer", 0)
	order()
	println(branches(1), branches(2), branches(3))
}

func main() {
	nested()
	println(pointers())
}
