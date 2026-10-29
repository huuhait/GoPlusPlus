// want: error
// What cannot be embedded: a pointer to an interface, a type parameter.
package main

type T struct{}
type I interface{ M() }

type P2 struct{ *I }
type G[X any] struct{ X }

func main() {}
