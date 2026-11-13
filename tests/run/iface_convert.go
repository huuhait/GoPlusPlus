package main

type Shape interface {
	Area() int
}

type Named interface {
	Name() string
}

type NamedShape interface {
	Shape
	Named
}

type Scaler interface {
	Scale(by int)
}

type hidden interface {
	secret() int
}

type Square struct{ side int }

func (s Square) Area() int    { return s.side * s.side }
func (s Square) Name() string { return "square" }
func (s Square) secret() int  { return 42 }

type Circle struct{ r int }

func (c *Circle) Area() int    { return 3 * c.r * c.r }
func (c *Circle) Name() string { return "circle" }
func (c *Circle) Scale(by int) { c.r *= by }

type Dot struct{}

func (Dot) Area() int { return 0 }

type Num int

func (n Num) Name() string { return "num" }

func describe(v any) string {
	switch x := v.(type) {
	case NamedShape:
		return "named shape " + x.Name()
	case Shape:
		if x.Area() == 0 {
			return "empty shape"
		}
		return "shape"
	case Named:
		return "named " + x.Name()
	case nil:
		return "nil"
	case int, string:
		return "plain"
	}
	return "other"
}

func name(s Shape) string {
	if n, ok := s.(Named); ok {
		return n.Name()
	}
	return "nameless"
}

func widen(n NamedShape) (Shape, Named, any) {
	return n, n, n
}

func scale(v any, by int) bool {
	s, ok := v.(Scaler)
	if ok {
		s.Scale(by)
	}
	return ok
}

func both() (NamedShape, int) { return Square{2}, 1 }
func plain() (Square, string) { return Square{3}, "x" }
func passed() (Shape, any)    { return plain() }
func renamed() (Named, Shape) { n, _ := both(); return n, n }

// The results of one call go to places of other interface types.
func results() {
	var named Named
	var shape Shape
	var held any
	var n int
	var text string
	named, n = both()
	println(named.Name(), n)
	shape, text = plain()
	println(shape.Area(), text)
	held, n = both()
	_, isSquare := held.(Square)
	println(isSquare, held.(Named).Name())
	shape, held = passed()
	println(shape.Area(), held.(string))
	named, shape = renamed()
	println(named.Name(), shape.Area())
	var first, second any = plain()
	println(first.(Square).side, second.(string))
}

func main() {
	results()
	c := Circle{1}
	values := []any{Square{2}, &c, Dot{}, Num(1), 7, "text", nil, 1.5, c}
	for _, v := range values {
		println(describe(v))
	}
	println(name(Square{1}), name(&c), name(Dot{}))

	var ns NamedShape = Square{3}
	s, n, a := widen(ns)
	println(s.Area(), n.Name(), a.(NamedShape).Area(), a.(Square).side)
	s = ns
	n = ns
	println(s.Area(), n.Name(), s == Shape(Square{3}), a == s, a == any(n))
	ns = &c
	s = ns
	println(s.Area(), s.(Named).Name(), s.(NamedShape).Name())

	println(scale(&c, 2), scale(Square{1}, 2), scale(nil, 2), c.r)
	var h hidden = Square{1}
	println(h.secret(), a.(hidden).secret())
	_, isHidden := any(Dot{}).(hidden)
	_, isAny := a.(any)
	var none any
	_, noneIsAny := none.(interface{})
	println(isHidden, isAny, noneIsAny)

	// Many types asked the same thing, more than once.
	count := 0
	for round := 0; round < 100; round++ {
		for _, v := range values {
			if _, ok := v.(Named); ok {
				count++
			}
			if _, ok := v.(Shape); ok {
				count += 10
			}
		}
	}
	println(count)
	shapes := []Shape{Square{1}, &c, Dot{}}
	names := ""
	for _, shape := range shapes {
		switch x := shape.(type) {
		case Scaler:
			x.Scale(2)
			names += "scaler "
		case Named:
			names += x.Name() + " "
		default:
			names += "? "
		}
	}
	println(names, c.r)
}
