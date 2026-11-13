package main

type Shape interface {
	Area() int
	Name() string
}

type Scaler interface{ Scale(by int) }

type Named interface{ Name() string }

type Base struct {
	w, h int
	tag  string
}

func (b Base) Area() int     { return b.w * b.h }
func (b Base) Name() string  { return "base " + b.tag }
func (b *Base) Scale(by int) { b.w *= by; b.h *= by }

// Embeds by value: the methods of Base, and one of its own over them.
type Box struct {
	Base
	depth int
}

func (b Box) Name() string { return "box " + b.tag }

// Embeds by pointer: also the methods that take the address.
type Ref struct {
	*Base
	id int
}

// Two levels down.
type Crate struct {
	Box
	label string
}

// Embeds an interface: its methods are called through the value there.
type Wrapped struct {
	Shape
	extra int
}

func (w Wrapped) Area() int { return w.Shape.Area() + w.extra }

type Titled struct {
	Named
	title string
}

func describe(s Shape) {
	println(s.Name(), s.Area())
}

func scales(v any) bool {
	s, ok := v.(Scaler)
	if ok {
		s.Scale(2)
	}
	return ok
}

func kind(v any) string {
	switch x := v.(type) {
	case Shape:
		return "shape " + x.Name()
	case Named:
		return "named " + x.Name()
	case Scaler:
		return "scaler"
	}
	return "none"
}

func main() {
	box := Box{Base{2, 3, "b"}, 1}
	describe(box)
	describe(&box)
	base := Base{1, 2, "r"}
	ref := Ref{&base, 7}
	describe(ref)
	describe(&ref)
	crate := Crate{box, "c"}
	describe(crate)
	wrapped := Wrapped{box, 10}
	describe(wrapped)
	describe(Wrapped{Wrapped{ref, 1}, 2})

	println(scales(box), scales(&box), scales(ref), scales(&ref), scales(crate), scales(&crate))
	println(box.w, base.w, crate.w, ref.Area())

	values := []any{box, &box, ref, crate, &crate, wrapped, Titled{box, "t"}, base, 7}
	for _, v := range values {
		println(kind(v))
	}

	var s Shape = crate
	if n, ok := s.(Named); ok {
		println(n.Name())
	}
	var named Named = Titled{ref, "x"}
	println(named.Name(), named.(Titled).title)
	_, isShape := named.(Shape)
	_, isScaler := any(wrapped).(Scaler)
	println(isShape, isScaler)
}
