// want: error
package main

type Adder[T any] interface {
	+ (T) T
}

func main() {}
