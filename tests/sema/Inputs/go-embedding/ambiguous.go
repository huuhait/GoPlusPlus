// want: error
// Two embedded types give the same name at the same depth: ambiguous.
package main

type A struct{ Name string }
type B struct{ Name string }
type C struct {
	A
	B
}

func (A) M() {}
func (B) M() {}

func main() {
	var c C
	_ = c.Name
	c.M()
	println(c.A.Name)
}
