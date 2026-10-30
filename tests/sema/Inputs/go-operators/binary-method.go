// want: error
// An operator method name is a syntax error in a .go file (TYP-27).
package main

type V struct{ X int }

func (a V) + (b V) V { return a }

func main() {}
