package main

type Counter struct {
	n    int
	name string
}

func (c *Counter) Inc() int             { c.n++; return c.n }
func (c Counter) Get() int              { return c.n }
func (c Counter) Label(s string) string { return c.name + s }

type Shape interface {
	Area() int
	Scale(by int)
}

type Square struct{ side int }

func (s *Square) Area() int    { return s.side * s.side }
func (s *Square) Scale(by int) { s.side *= by }

type Holder struct {
	c    Counter
	list []string
}

func (h *Holder) Add(s string) int {
	h.list = append(h.list, s)
	return len(h.list)
}

func apply(f func() int, times int) int {
	last := 0
	for i := 0; i < times; i++ {
		last = f()
	}
	return last
}

func incOf(start int) func() int {
	c := Counter{n: start}
	return c.Inc
}

func main() {
	c := Counter{n: 1, name: "c"}
	inc := c.Inc
	get := c.Get
	label := c.Label
	println(inc(), inc(), c.n, get(), label("!"))
	c.name = "changed"
	println(label("?"), c.Label("?"), c.Get())
	println(apply(c.Inc, 3), c.n, apply(c.Get, 2))

	p := &c
	pinc := p.Inc
	println(pinc(), c.n)

	h := Holder{c: Counter{n: 10}}
	add := h.Add
	hinc := h.c.Inc
	println(add("a"), add("b"), len(h.list), hinc(), h.c.n)

	kept := incOf(40)
	println(kept(), kept())

	sq := Square{2}
	var s Shape = &sq
	area := s.Area
	scale := s.Scale
	scale(3)
	println(area(), sq.side)
	other := Square{1}
	s = &other
	println(area(), s.Area())

	fs := []func() int{c.Get, p.Get, pinc, area}
	sum := 0
	for _, f := range fs {
		sum += f()
	}
	println(sum)

	getOf := Counter.Get
	labelOf := Counter.Label
	incOfPtr := (*Counter).Inc
	getOfPtr := (*Counter).Get
	println(getOf(c), labelOf(c, "."), incOfPtr(&c), getOfPtr(&c))
	var held any = c.Get
	if f, ok := held.(func() int); ok {
		println(f())
	}
}
