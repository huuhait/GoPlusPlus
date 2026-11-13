package main

type Ordered interface {
	~int | ~int8 | ~int64 | ~uint8 | ~float64 | ~string
}

type Number interface {
	~int | ~int64 | ~float64
}

type Stringer interface{ String() string }

type ID int

func (i ID) String() string { return "id" + digits(int(i)) }

type Point struct{ x, y int }

func (p Point) String() string { return "(" + digits(p.x) + "," + digits(p.y) + ")" }

func digits(n int) string {
	if n < 0 {
		return "-" + digits(-n)
	}
	if n < 10 {
		return string(rune('0' + n))
	}
	return digits(n/10) + string(rune('0'+n%10))
}

func Max[T Ordered](a, b T) T {
	if a > b {
		return a
	}
	return b
}

func Sum[T Number](xs []T) T {
	var total T
	for _, x := range xs {
		total += x
	}
	return total
}

func Map[T, U any](xs []T, f func(T) U) []U {
	out := make([]U, 0, len(xs))
	for _, x := range xs {
		out = append(out, f(x))
	}
	return out
}

func Filter[T any](xs []T, keep func(T) bool) []T {
	var out []T
	for _, x := range xs {
		if keep(x) {
			out = append(out, x)
		}
	}
	return out
}

func Reduce[T, A any](xs []T, start A, step func(A, T) A) A {
	acc := start
	for _, x := range xs {
		acc = step(acc, x)
	}
	return acc
}

func Index[T comparable](xs []T, want T) int {
	for i, x := range xs {
		if x == want {
			return i
		}
	}
	return -1
}

func Keys[K comparable, V any](m map[K]V) int {
	n := 0
	for range m {
		n++
	}
	return n
}

func Zero[T any]() T {
	var zero T
	return zero
}

func First[T any](xs []T) (T, bool) {
	if len(xs) == 0 {
		var zero T
		return zero, false
	}
	return xs[0], true
}

func Of[T any](xs ...T) []T { return xs }

func Twice[T Number](x T) T { return Sum([]T{x, x}) }

func Depth[T any](xs []T, n int) int {
	if n == 0 {
		return len(xs)
	}
	return Depth(append(xs, xs...), n-1)
}

func Join[T Stringer](xs []T, sep string) string {
	out := ""
	for i, x := range xs {
		if i > 0 {
			out += sep
		}
		out += x.String()
	}
	return out
}

func Kind[T any](x T) string {
	var held any = x
	switch v := held.(type) {
	case int:
		return "int " + digits(v)
	case string:
		return "string " + v
	case Stringer:
		return "stringer " + v.String()
	case nil:
		return "nil"
	}
	return "other"
}

func Scale[T Number](xs []T, by T) []T {
	scale := func(x T) T { return x * by }
	return Map(xs, scale)
}

func Counter[T Number](step T) func() T {
	var n T
	return func() T {
		n += step
		return n
	}
}

func Last[T any](xs []T, note *string) (last T) {
	defer func() {
		if r := recover(); r != nil {
			*note = "empty"
		}
	}()
	return xs[len(xs)-1]
}

func Convert[T, U Number](x T) U { return U(x) + U(1) }

func Swap[A, B any](a A, b B) (B, A) { return b, a }

func Double[T Number](x T) T { return x + x }

func Apply[T any](x T, steps ...func(T) T) T {
	for _, step := range steps {
		x = step(x)
	}
	return x
}

func Pick[T Number](twice bool) func(T) T {
	if twice {
		return Double[T]
	}
	return func(x T) T { return x }
}

type Pair[K comparable, V any] struct {
	key K
	val V
}

func main() {
	println(Max(3, 7), Max("pear", "apple"), Max(2.5, 1.5), Max[ID](4, 9))
	println(Sum([]int{1, 2, 3}), Sum[float64]([]float64{1.5, 2}), Twice(ID(4)), Sum([]int{4}))
	words := Map([]int{1, 22, 333}, digits)
	println(len(words), words[2], len(Map(words, func(s string) int { return len(s) })))
	even := Filter([]int{1, 2, 3, 4, 5, 6}, func(n int) bool { return n%2 == 0 })
	println(len(even), even[2], Reduce(even, "", func(acc string, n int) string { return acc + digits(n) }))
	println(Index(words, "22"), Index([]float64{1, 2}, 3), Index([]Point{{1, 2}, {3, 4}}, Point{3, 4}))
	println(Keys(map[string]int{"a": 1, "b": 2}), Keys(map[ID]Point{}))
	println(Zero[int](), Zero[string]() == "", Zero[*Point]() == nil, Zero[Point]().x, Zero[[]int]() == nil)
	first, ok := First(words)
	none, missing := First([]Point(nil))
	println(first, ok, none.x, missing)
	println(len(Of[int]()), len(Of(1, 2, 3)), Of("a", "b")[1], len(Of(words...)))
	println(Depth([]int{1}, 3), Depth([]string{"a", "b"}, 2))
	println(Join([]ID{1, 2}, "+"), Join([]Point{{1, 2}}, ""), Join([]Stringer{ID(3), Point{4, 5}}, " "))
	println(Kind(1), Kind("s"), Kind(ID(2)), Kind(Point{}), Kind(2.5), Kind[Stringer](ID(7)), Kind[any](nil))
	scaled := Scale([]float64{1, 2}, 1.5)
	println(scaled[0], scaled[1], Scale([]int{2, 3}, 4)[1])
	next, half := Counter(2), Counter(0.5)
	println(next(), next(), half(), half(), next())
	note := ""
	println(Last([]int{1, 2}, &note), Last([]string{}, &note) == "", note)
	println(Convert[int, float64](2), Convert[float64, int](2.75), Convert[ID, int64](5))
	s, n := Swap(1, "one")
	p, q := Swap(Point{1, 2}, []int{3})
	println(s, n, len(p), q.y)
	double := Double[int]
	var twice func(float64) float64 = Double
	println(double(4), twice(1.25), len(Map([]int{1, 2}, Double[int])), Map([]ID{3}, Double)[0])
	println(Apply(1, double, Double[int], Pick[int](true), Pick[int](false)), Apply("x"), Pick[float64](true)(0.5))
}
