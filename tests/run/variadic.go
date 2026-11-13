package main

type Named struct {
	name string
	tags []string
}

func sum(xs ...int) int {
	total := 0
	for _, x := range xs {
		total += x
	}
	return total
}

func join(sep string, parts ...string) string {
	out := ""
	for i, part := range parts {
		if i > 0 {
			out += sep
		}
		out += part
	}
	return out
}

func count(xs ...int) (int, bool) { return len(xs), xs == nil }

var kept []string

func keep(parts ...string) int {
	kept = append(kept, parts...)
	return len(kept)
}

func hold(parts ...string) []string { return parts }

func kinds(values ...any) string {
	out := ""
	for _, value := range values {
		switch v := value.(type) {
		case int:
			out += "int "
		case string:
			out += "string:" + v + " "
		case nil:
			out += "nil "
		case []int:
			out += "ints "
		default:
			out += "other "
		}
	}
	return out
}

func names(all ...Named) string {
	out := ""
	for _, one := range all {
		out += one.name + join("+", one.tags...) + ";"
	}
	return out
}

func nested(rows ...[]int) int {
	total := 0
	for _, row := range rows {
		total += sum(row...)
	}
	return total
}

type Log struct {
	prefix string
	lines  []string
}

func (l *Log) Add(lines ...string) int {
	for _, line := range lines {
		l.lines = append(l.lines, l.prefix+line)
	}
	return len(l.lines)
}

func (l Log) Has(wanted ...string) int {
	found := 0
	for _, want := range wanted {
		for _, line := range l.lines {
			if line == l.prefix+want {
				found++
			}
		}
	}
	return found
}

type Adder interface {
	Add(lines ...string) int
}

var order []string

func step(name string) string {
	order = append(order, name)
	return name
}

func number(name string, n int) int {
	order = append(order, name)
	return n
}

func pair() (string, string) { return "left", "right" }

func main() {
	println(sum(), sum(1), sum(1, 2, 3), sum([]int{4, 5, 6}...))
	println(join("-"), join("-", "a"), join("-", "a", "b", "c"))
	n, none := count()
	println(n, none)
	n, none = count(1, 2)
	println(n, none)
	n, none = count(nil...)
	println(n, none)
	n, none = count([]int{}...)
	println(n, none)

	a, b := "x", "y"
	println(keep(a, b), keep(), keep(a+b))
	println(join(",", kept...))
	held := hold(a, b, a+b)
	println(len(held), cap(held), held[2])
	parts := []string{"p", "q"}
	same := hold(parts...)
	println(len(same), same[1])

	var empty any
	println(kinds(1, "two", empty, []int{3}, 4.5))
	println(kinds())
	println(names(Named{"n", []string{"t", "u"}}, Named{name: "m"}))
	println(nested([]int{1, 2}, nil, []int{3}))

	logged := Log{prefix: "> "}
	log := &logged
	println(log.Add("one", "two"), log.Add(), log.Add(parts...))
	println(log.Has("one", "p", "none"), log.Has())
	var adder Adder = log
	println(adder.Add("three"), adder.Add(parts...), len(log.lines))

	add := log.Add
	println(add("four", "five"))
	total := sum
	println(total(1, 2), total(), total([]int{7, 8}...))
	joiner := func(sep string, parts ...string) string { return "[" + join(sep, parts...) + "]" }
	println(joiner("/", "a", "b"), joiner("/"))

	println(join(step("sep"), step("first"), step("second")), sum(number("one", 1), number("two", 2)))
	println(join(",", order...))
	println(join(pair()))

	func() {
		defer println(sum(1, 2), "deferred")
		defer keep("late", a)
		defer log.Add("last")
		defer func(xs ...int) { println("literal", len(xs)) }(1, 2, 3)
	}()
	println(len(kept), len(log.lines), log.lines[len(log.lines)-1])
}
