package main

// Legal in Go (types2 go.dev/issue/48703): a method signature names a bigger
// instance of its own type; only bodies count for the instantiation cycle.
type Pair[T any] struct{ n int }

func (p Pair[T]) Nest() Pair[Pair[T]] { panic(0) }
func (p Pair[T]) Items() []T          { panic(0) }

func f(p *Pair[int]) {}

func main() {}
