// want: ok
// A field at a shallower depth shadows a promoted one; the inner one is
// still reached by its path.
package main

type Inner struct{ Name string; N int }
type Mid struct {
	Inner
	Name int
}
type Outer struct {
	Mid
	N string
}

func main() {
	var o Outer
	o.Name = 1
	o.N = "x"
	o.Mid.Inner.Name = "y"
	o.Inner.N = 2
	println(o.Name, o.N, o.Mid.Inner.Name, o.Inner.N)
}
