package main

type pair struct {
	name string
	n    int
}

func build(n int) string {
	s := ""
	for i := 0; i < n; i++ {
		s += "ab"
	}
	return s
}

func main() {
	// A string that is built piece by piece.
	long := build(20000)
	println(len(long), long[0:4], long[len(long)-2:])
	// A string that shares its bytes is left as it is.
	a := "start"
	a += "-one"
	b := a
	a += "-two"
	println(a, b)
	part := a[0:5]
	a += "!"
	println(a, part, b)
	// Itself, a part of itself, nothing.
	c := "xy"
	c += "z"
	c += c
	c += c[1:3]
	c += ""
	println(c, len(c))
	// In a field, in an element, through a pointer.
	p := pair{name: "p"}
	p.name += "q"
	p.name += "r"
	names := []string{"a", "b"}
	names[1] += "c"
	names[1] += "d"
	q := &p
	q.name += "s"
	println(p.name, names[0], names[1])
	// A literal is never written to.
	lit := "lit"
	lit += "eral"
	again := "lit"
	println(lit, again)
}
