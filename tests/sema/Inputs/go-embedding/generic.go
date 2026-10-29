// want: ok
// An embedded generic instance and an embedded alias; promotion through
// them, and an embedded interface.
package main

type Box[T any] struct{ V T }

func (b *Box[T]) Set(v T) { b.V = v }

type Pt struct{ X, Y int }
type Alias = Pt

type Shape interface{ Area() int }
type sq struct{ s int }

func (q sq) Area() int { return q.s * q.s }

type S struct {
	Box[int]
	Alias
	Shape
}

func main() {
	s := S{Shape: sq{2}}
	s.Set(3)
	s.X = 1
	println(s.V, s.X, s.Area())
}
