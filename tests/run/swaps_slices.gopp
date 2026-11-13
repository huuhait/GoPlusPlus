package main

func swap(xs [][]int, i, j int) [][]int {
	xs[i], xs[j] = xs[j], xs[i]
	return xs
}

func turn(xs [][]int, i, j, k int) [][]int {
	xs[i], xs[j], xs[k] = xs[j], xs[k], xs[i]
	return xs
}

func show(xs [][]int) {
	println(len(xs[0]), len(xs[1]), len(xs[2]), xs[0][0], xs[1][0], xs[2][0])
}

func main() {
	xs := [][]int{{1}, {2, 2}, {3, 3, 3}}
	xs = swap(xs, 0, 1)
	show(xs)
	xs = swap(xs, 2, 2)
	show(xs)
	xs = turn(xs, 0, 1, 2)
	show(xs)
	xs = turn(xs, 0, 0, 2)
	show(xs)
	xs = turn(xs, 1, 1, 1)
	show(xs)
	a, b, c := []int{1}, []int{2, 2}, []int{3, 3, 3}
	a, b, c = b, c, a
	println(len(a), len(b), len(c))
	m := map[string][]int{"a": {1}, "b": {2, 2}}
	m["a"], m["b"] = m["b"], m["a"]
	println(len(m["a"]), len(m["b"]))
}
