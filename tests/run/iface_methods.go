package main

type Shape interface {
	Area() int
	Name() string
}

type Scaler interface {
	Scale(by int)
}

type Square struct{ side int }

func (s Square) Area() int    { return s.side * s.side }
func (s Square) Name() string { return "square" }

type Rect struct {
	w, h int
	tag  string
}

func (r Rect) Area() int     { return r.w * r.h }
func (r Rect) Name() string  { return "rect " + r.tag }
func (r *Rect) Scale(by int) { r.w *= by; r.h *= by }

type Circle struct{ r int }

func (c *Circle) Area() int      { return 3 * c.r * c.r }
func (c *Circle) Name() string   { return "circle" }
func (c *Circle) Scale(by int)   { c.r *= by }
func (c *Circle) String() string { return "circle!" }

// A type of one word that is no struct.
type Side int

func (s Side) Area() int    { return int(s) * int(s) }
func (s Side) Name() string { return "side" }

type Labels []string

func (l Labels) Area() int    { return len(l) }
func (l Labels) Name() string { return "labels " + l[0] }

type Joiner interface {
	Join(sep string, more []string) string
	Count(extra []int) int
}

type Words struct{ first string }

func (w Words) Join(sep string, more []string) string {
	out := w.first
	for _, word := range more {
		out += sep + word
	}
	return out
}

func (w Words) Count(extra []int) int { return len(extra) }

type Pair interface {
	Both() (int, string)
}

func (s Square) Both() (int, string) { return s.side, "sq" }

func describe(s Shape) {
	println(s.Name(), s.Area())
}

func total(shapes []Shape) int {
	sum := 0
	for _, s := range shapes {
		sum += s.Area()
	}
	return sum
}

func grow(s Scaler, by int) { s.Scale(by) }

func kind(s Shape) string {
	switch v := s.(type) {
	case Square:
		return "Square " + v.Name()
	case Rect:
		return "Rect " + v.tag
	case *Circle:
		return "*Circle " + v.String()
	case nil:
		return "nil"
	}
	return "other " + s.Name()
}

type holder struct {
	shape Shape
	count int
}

func main() {
	var s Shape = Square{3}
	describe(s)
	s = Rect{2, 5, "a"}
	describe(s)
	c := Circle{2}
	s = &c
	describe(s)
	describe(Side(4))
	describe(Labels{"x", "y"})

	shapes := []Shape{Square{1}, Rect{2, 3, "b"}, &c, Side(2), Labels{"l"}}
	println(total(shapes))

	r := Rect{1, 2, "r"}
	grow(&r, 3)
	grow(&c, 2)
	println(r.w, r.h, c.r, s.Area())

	for _, shape := range shapes {
		println(kind(shape))
	}
	var none Shape
	println(kind(none), none == nil, s != nil)

	if sq, ok := shapes[0].(Square); ok {
		println("square of", sq.side)
	}
	if _, ok := shapes[1].(Square); !ok {
		println("no square")
	}
	println(shapes[1].(Rect).tag, shapes[2].(*Circle).r)

	var a any = s
	_, isCircle := a.(*Circle)
	_, isSquare := a.(Square)
	println(isCircle, isSquare)
	a = shapes[0]
	println(a.(Square).side, a == any(Square{1}), a == any(Square{2}))
	a = none
	println(a == nil)

	println(shapes[0] == Shape(Square{1}), shapes[0] == shapes[1], shapes[2] == s, shapes[3] == Shape(Side(2)))

	var j Joiner = Words{"a"}
	println(j.Join("-", []string{"b", "c"}), j.Count(nil), j.Count([]int{1, 2, 3}))
	var p Pair = Square{9}
	n, text := p.Both()
	println(n, text)

	h := holder{shape: Rect{4, 4, "held"}}
	h.count = h.shape.Area()
	println(h.count, h.shape.Name())
	byName := map[string]Shape{}
	for _, shape := range shapes {
		byName[shape.Name()] = shape
	}
	println(len(byName), byName["square"].Area(), byName["circle"].Area())
}
