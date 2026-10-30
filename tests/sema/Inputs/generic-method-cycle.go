package main

type U[T any] []int

func (U[T]) m() *U[*T] { panic("unused") }

type G[T any] struct{}

func (x *G[T]) Next() *G[*T] { panic("unused") }

func main() {
	var x G[int]
	_ = x
}
