package main

type Acc struct {
	total int
	notes []string
}

func (a *Acc) add(n int, note string) {
	a.total += n
	a.notes = append(a.notes, note)
}

func apply(values []int, f func(int)) {
	for _, v := range values {
		f(v)
	}
}

func visit(values []int, f func(i, v int) bool) int {
	for i, v := range values {
		if !f(i, v) {
			return i
		}
	}
	return len(values)
}

func keep(f func(int)) func(int) { return f }

// Called where it is written.
func direct(n int) (int, string) {
	sum := n
	text := "a"
	func() {
		sum *= 2
		text += "b"
		func() { sum++; text += "c" }()
	}()
	return sum, text
}

// Passed to a function that only calls it.
func lent(values []int) (int, int, string) {
	sum := 0
	var acc Acc
	apply(values, func(v int) {
		sum += v
		acc.add(v, "n")
	})
	stopped := visit(values, func(i, v int) bool { return v < 3 })
	return sum + acc.total, stopped, acc.notes[len(acc.notes)-1]
}

// A variable that holds a literal and is only called.
func helper(values []int) int {
	sum := 0
	add := func(v int) { sum += v }
	twice := func(v int) {
		add(v)
		add(v)
	}
	for _, v := range values {
		if v%2 == 0 {
			twice(v)
		} else {
			add(v)
		}
	}
	return sum
}

// A literal that goes on living shares its variables with one that
// does not.
func mixed(values []int) (int, int) {
	sum := 0
	add := func(v int) { sum += v }
	later := keep(add)
	apply(values, func(v int) { sum += v * 10 })
	later(1)
	func() { sum += 100 }()
	return sum, len(values)
}

func loops() int {
	total := 0
	for i := 0; i < 4; i++ {
		func() { total += i }()
		step := func(by int) { total += by * i }
		step(10)
	}
	for _, v := range []int{5, 6} {
		apply([]int{1}, func(one int) { total += v * one })
	}
	return total
}

func deferred(n int) (out int, text string) {
	defer func() {
		out += n
		text += "!"
	}()
	text = "set"
	return 1, text + "+"
}

func main() {
	xs := []int{1, 2, 3, 4}
	sum, text := direct(3)
	println(sum, text)
	a, b, c := lent(xs)
	println(a, b, c)
	println(helper(xs))
	d, e := mixed(xs)
	println(d, e)
	println(loops())
	out, note := deferred(5)
	println(out, note)
}
