package main

type doc struct {
	title string
	tags  []string
	seen  map[string]int
}

var shelf []doc
var lists [][]int
var words []string

func keep(xs []int) { lists = append(lists, xs) }
func file(d doc)    { shelf = append(shelf, d) }
func say(s string)  { words = append(words, s) }

func total(xs []int) int {
	t := 0
	for _, x := range xs {
		t += x
	}
	return t
}

func straight() {
	a := []int{1, 2, 3}
	keep(a)
	b := []int{4, 5}
	keep(b)
	println(total(b))
	c := b
	println(total(c))
	s := "one"
	s += "-two"
	say(s)
	t := s + "!"
	say(t)
}

func branches(n int) {
	xs := []int{n, n}
	if n%2 == 0 {
		keep(xs)
	} else {
		keep(xs)
	}
	ys := []int{n}
	if n > 1 {
		keep(ys)
	}
	println(len(ys), ys[0])
	zs := []int{n, n, n}
	if n == 1 {
		keep(zs)
		return
	}
	println(total(zs))
}

func loops(n int) {
	for i := 0; i < n; i++ {
		w := []int{i, i}
		keep(w)
	}
	e := []int{9}
	for i := 0; i < n; i++ {
		keep(e)
	}
	f := []int{7, 7}
	for i := 0; i < n; i++ {
		if i == 1 {
			keep(f)
			break
		}
	}
	g := []int{1}
	for i := 0; i < n; i++ {
		keep(g)
		g = []int{i, i, i}
	}
	println(len(g))
}

func values() {
	d := doc{title: "a", tags: []string{"x", "y"}, seen: map[string]int{"x": 1}}
	file(d)
	println(d.title, len(d.tags), len(d.seen))
	e := d
	file(e)
	m := map[string][]int{"k": {1, 2}}
	for k, v := range m {
		keep(v)
		say(k)
	}
	names := []string{"p", "q"}
	for _, name := range names {
		say(name)
	}
	say(names[0])
}

func results() (out []int, n int) {
	out = []int{1, 2}
	keep(out)
	tmp := []int{3}
	keep(tmp)
	n = len(out)
	return
}

func main() {
	straight()
	branches(1)
	branches(2)
	branches(3)
	loops(3)
	values()
	out, n := results()
	println(len(out), n)
	println(len(lists), len(shelf), len(words))
	sum := 0
	for _, xs := range lists {
		sum += total(xs)
	}
	println(sum)
	for _, d := range shelf {
		println(d.title, len(d.tags), d.seen["x"])
	}
	for _, w := range words {
		println(w)
	}
}
