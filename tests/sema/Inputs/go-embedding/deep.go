// want: error
// A promoted method through two levels, and one that is not there.
package main

type A struct{}

func (A) Hello() string { return "a" }

type B struct{ A }
type C struct{ B }

func main() {
	var c C
	println(c.Hello())
	println(c.Bye())
}
