// exit: 2
package main

type Color struct{ r, g, b uint8 }

func (c *Color) String() string { return "a color" }

func main() {
	defer func() {
		println("deferred")
	}()
	c := Color{1, 2, 3}
	panic(&c)
}
