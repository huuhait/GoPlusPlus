package main

// A panic is raised at every call of `step`, one after the other, and
// stopped in `attempt`: the program goes on each time as in Go.

var countdown int

func step() {
	countdown--
	if countdown == 0 {
		panic("step")
	}
}

func text(n int) string {
	s := "t"
	for i := 0; i < n; i++ {
		s += "x"
		step()
	}
	return s
}

func list(n int) []string {
	var out []string
	for i := 0; i < n; i++ {
		out = append(out, text(i))
	}
	return out
}

func join(a, b string) string {
	step()
	return a + "+" + b
}

type Named interface{ Name() string }

type Person struct{ name string }

func (p Person) Name() string {
	step()
	return "person " + p.name
}

type Box struct {
	label string
	items []string
}

func (b *Box) add(s string) {
	step()
	b.items = append(b.items, s)
}

func scopes() int {
	total := len(join(text(2), text(3)))
	{
		b := Box{label: text(1)}
		b.add(text(2))
		b.add(join("a", "b"))
		total += len(b.items) + len(b.label)
	}
	names := list(3)
	for i, name := range names {
		if i == 1 {
			continue
		}
		total += len(name)
		step()
	}
	switch v := any(text(total % 3)).(type) {
	case string:
		step()
		total += len(v)
	}
	return total
}

func maps() int {
	m := map[string][]string{}
	m["a"] = list(2)
	step()
	m["b"] = list(1)
	total := 0
	for key, values := range m {
		step()
		total += len(key) + len(values)
	}
	return total
}

func values() int {
	prefix := text(2)
	count := 0
	add := func(s string) string {
		step()
		count++
		return prefix + s
	}
	keep := add
	out := keep(text(1))
	var n Named = Person{name: out}
	step()
	var held any = text(3)
	s, _ := held.(string)
	say := n.Name
	return len(say()) + len(s) + count
}

func deferred() (out string) {
	defer func(note string) {
		step()
		out += note
	}(text(2))
	defer join(text(1), "kept")
	step()
	out = text(3)
	return out
}

// A panic in a deferred call, which a deferred call before it stops:
// the function returns what it had.
func inner() (n int) {
	defer func() {
		if recover() != nil {
			n += 1000
		}
	}()
	defer step()
	held := list(2)
	n = len(held)
	step()
	return n
}

func pair() (string, []string) {
	a := text(2)
	b := list(2)
	a = join(a, text(1))
	step()
	return a, b
}

func work() int {
	total := scopes()
	step()
	total += maps()
	total += values()
	total += len(deferred())
	total += inner()
	a, b := pair()
	step()
	return total + len(a) + len(b)
}

func attempt() (result int, stopped bool) {
	defer func() {
		if recover() != nil {
			stopped = true
		}
	}()
	held := text(1)
	result = work() + len(held)
	return result, false
}

func main() {
	stops, sum := 0, 0
	for target := 1; target < 400; target++ {
		countdown = target
		result, stopped := attempt()
		sum += result
		if !stopped {
			println("no panic from", target, "on")
			break
		}
		stops++
	}
	println("stopped", stops, "panics", sum)
	countdown = 0
	result, _ := attempt()
	println(result)
}
