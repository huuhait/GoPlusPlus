// want: error
package main

type T struct{}

func (t T) range () func(yield func(int) bool) { return nil }

func main() {}
