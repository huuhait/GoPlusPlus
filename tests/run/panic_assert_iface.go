// exit: 2
package main

type Shape interface {
	Area() int
	Name() string
}

type Square struct{ side int }

func (s Square) Area() int { return s.side * s.side }

type Named struct{ name string }

func (n Named) Area() int    { return 0 }
func (n Named) Name() string { return n.name }

func shape(v any) Shape { return v.(Shape) }

func main() {
	println(shape(Named{"n"}).Name())
	println(shape(Square{2}).Area())
}
