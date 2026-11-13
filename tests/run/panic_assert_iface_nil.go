// exit: 2
package main

type Shape interface{ Area() int }

type Namer interface{ Name() string }

func namer(s Shape) Namer { return s.(Namer) }

func main() {
	var s Shape
	println(namer(s) == nil)
}
