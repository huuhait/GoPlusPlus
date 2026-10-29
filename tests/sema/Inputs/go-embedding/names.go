// want: ok
// `extends` and `base` are ordinary names in a .go file (TYP-26), also as
// the names of embedded types.
package main

type extends struct{ base int }
type base struct{ extends int }

type both struct {
	extends
	base
}

func extendsF(base int) int { return base }

func main() {
	var b both
	b.extends.base = 1
	b.base.extends = 2
	var extends = extendsF(b.extends.base)
	println(extends, b.base.extends)
}
