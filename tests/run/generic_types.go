package main

type Number interface{ ~int | ~float64 }

type Stack[T any] struct{ items []T }

func (s *Stack[T]) Push(x T) { s.items = append(s.items, x) }

func (s *Stack[T]) Pop() (T, bool) {
	var zero T
	if len(s.items) == 0 {
		return zero, false
	}
	last := s.items[len(s.items)-1]
	s.items = s.items[:len(s.items)-1]
	return last, true
}

func (s Stack[T]) Len() int { return len(s.items) }

func (s Stack[T]) Each(visit func(int, T)) {
	for i, x := range s.items {
		visit(i, x)
	}
}

type Pair[K, V comparable] struct {
	Key K
	Val V
}

func (p Pair[K, V]) Swap() Pair[V, K] { return Pair[V, K]{p.Val, p.Key} }

func (p Pair[K, V]) Is(key K) bool { return p.Key == key }

func MakePair[K, V comparable](k K, v V) Pair[K, V] { return Pair[K, V]{k, v} }

type Vec[T Number] []T

func (v Vec[T]) Sum() T {
	var total T
	for _, x := range v {
		total += x
	}
	return total
}

func (v Vec[T]) Scale(by T) Vec[T] {
	out := make(Vec[T], len(v))
	for i, x := range v {
		out[i] = x * by
	}
	return out
}

type Table[K comparable, V any] map[K]V

func (t Table[K, V]) Get(key K, or V) V {
	if v, ok := t[key]; ok {
		return v
	}
	return or
}

type Sized interface{ Len() int }

type Named interface{ Name() string }

type Box[T any] struct{ held T }

func (b Box[T]) Name() string { return "box" }

func (b *Box[T]) Set(x T) { b.held = x }

func (b Box[T]) Get() T { return b.held }

func Boxed[T any](x T) Box[T] {
	var b Box[T]
	b.Set(x)
	return b
}

func name(n Named) string { return n.Name() }

type Wrap[T any] struct {
	Box[T]
	count int
}

type Grid[T Number] struct {
	rows  []Vec[T]
	cells map[string]Pair[string, T]
}

func (g *Grid[T]) Add(name string, row Vec[T]) T {
	g.rows = append(g.rows, row)
	if g.cells == nil {
		g.cells = map[string]Pair[string, T]{}
	}
	g.cells[name] = Pair[string, T]{name, row.Sum()}
	return g.cells[name].Val
}

func (g Grid[T]) Total() T {
	var total T
	for _, row := range g.rows {
		total += row.Sum()
	}
	return total
}

func main() {
	var s Stack[string]
	s.Push("a")
	s.Push("b")
	top, ok := s.Pop()
	println(s.Len(), top, ok)
	var ints Stack[int]
	ints.Push(4)
	ints.Push(5)
	sum := 0
	ints.Each(func(i, x int) { sum += i * x })
	_, _ = ints.Pop()
	_, _ = ints.Pop()
	none, found := ints.Pop()
	println(sum, none, found, ints.Len())

	p := MakePair("one", 1)
	q := p.Swap()
	println(p.Key, q.Key, q.Val, p.Is("one"), q.Is(2), Pair[int, bool]{3, true}.Swap().Key)

	v := Vec[int]{1, 2, 3}
	println(v.Sum(), v.Scale(2).Sum(), Vec[float64]{0.5, 0.25}.Scale(4).Sum())

	t := Table[string, int]{"a": 1}
	println(t.Get("a", 9), t.Get("z", 9), Table[int, string]{}.Get(1, "none"))

	var sized Sized = s
	var any1 any = Box[int]{held: 7}
	b, isBox := any1.(Box[int])
	_, isOther := any1.(Box[string])
	println(sized.Len(), name(Box[string]{}), b.Get(), isBox, isOther)
	if n, ok := any1.(Named); ok {
		println(n.Name(), Boxed("x").Get(), Boxed(2.5).Get())
	}

	w := Wrap[string]{count: 2}
	w.Set("held")
	println(w.Get(), w.Name(), name(w), w.count)

	var g Grid[float64]
	println(g.Add("a", Vec[float64]{1, 2}), g.Add("b", Vec[float64]{0.5}), g.Total(), len(g.cells))
	get := b.Get
	push := s.Push
	push("c")
	println(get(), s.Len())
}
