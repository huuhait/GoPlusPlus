package main

type A[T any] = float64
type B[T any] = int
type L[T any] = []T

func main() {
	var a A[int] = 1.5
	var b B[string] = 2
	var c L[int]
	_, _, _ = a, b, c
}
