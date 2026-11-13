package main

type Op func(a, b int) int

type Named struct {
	name string
	run  func(string) string
}

type Shape interface{ Area() int }

type Square struct{ side int }

func (s Square) Area() int { return s.side * s.side }

func add(a, b int) int { return a + b }
func sub(a, b int) int { return a - b }

func shout(s string) string { return s + "!" }

func keep(list []string, s string) []string { return append(list, s) }

func apply(op Op, a, b int) int { return op(a, b) }

func twice(f func(string) string, s string) string { return f(f(s)) }

func pick(minus bool) Op {
	if minus {
		return sub
	}
	return add
}

func fold(values []int, start int, op func(int, int) int) int {
	for _, v := range values {
		start = op(start, v)
	}
	return start
}

func each(shapes []Shape, visit func(Shape) int) int {
	sum := 0
	for _, s := range shapes {
		sum += visit(s)
	}
	return sum
}

func pair() (func(int) int, func() string) {
	return func(n int) int { return n * 2 }, func() string { return "second" }
}

func main() {
	println(apply(add, 2, 3), apply(sub, 2, 3), apply(pick(true), 9, 4), pick(false)(9, 4))
	var op Op = add
	println(op(1, 1), op != nil)
	op = func(a, b int) int { return a * b }
	println(op(6, 7), fold([]int{1, 2, 3, 4}, 0, add), fold([]int{1, 2, 3, 4}, 1, op))

	println(twice(shout, "hey"), twice(func(s string) string { return "<" + s + ">" }, "x"))
	n := Named{"n", shout}
	println(n.run(n.name), Named{run: func(s string) string { return s + s }}.run("ab"))

	ops := []Op{add, sub, func(a, b int) int { return a % b }}
	for i, each := range ops {
		println(i, each(17, 5))
	}
	byName := map[string]func(int, int) int{"add": add, "sub": sub}
	println(byName["add"](1, 2), byName["sub"](1, 2), len(byName))

	double, text := pair()
	println(double(21), text(), func() int { return 7 }())
	println(func(a int) func(int) int {
		return func(b int) int { return b + 1 }
	}(1)(41))

	var held any = add
	if f, ok := held.(func(int, int) int); ok {
		println(f(20, 22))
	}
	_, isOp := held.(Op)
	println(isOp)

	shapes := []Shape{Square{1}, Square{2}}
	println(each(shapes, func(s Shape) int { return s.Area() }))
	list := keep(nil, "a")
	grow := keep
	list = grow(list, "b")
	println(len(list), list[1])

	var none func()
	println(none == nil)
	none = func() { println("called") }
	none()
	println(none != nil)
}
