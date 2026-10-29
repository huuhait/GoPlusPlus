// want: error
// A pointer to a pointer is not an embedded field: a syntax error.
package main

type T struct{}

type P1 struct{ **T }

func main() {}
