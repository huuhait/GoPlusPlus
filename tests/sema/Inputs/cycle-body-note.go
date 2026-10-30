package main

type U[T any] []int

func (U[T]) m() { var _ U[*T] }

func main() {}
