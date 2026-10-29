// want: error
// The same type embedded twice, and an embedded type next to a field of its
// name.
package main

type T struct{ X int }

type S1 struct {
	T
	T
}

type S2 struct {
	T
	T int
}

func main() {}
