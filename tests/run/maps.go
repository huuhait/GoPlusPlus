package main

type point struct{ x, y int32 }

func count(words []string) map[string]int {
	seen := map[string]int{}
	for _, word := range words {
		seen[word]++
	}
	return seen
}

func main() {
	var none map[string]int
	println(len(none), none == nil, none["a"])
	v, ok := none["a"]
	println(v, ok)

	ages := map[string]int{"ann": 31, "bob": 27}
	ages["cat"] = 40
	ages["bob"] += 3
	println(len(ages), ages["ann"], ages["bob"], ages["cat"], ages["dan"])
	if age, ok := ages["cat"]; ok {
		println("cat", age)
	}
	_, ok = ages["dan"]
	println(ok)
	delete(ages, "ann")
	delete(ages, "nobody")
	println(len(ages), ages["ann"])

	sum := 0
	keys := 0
	for name, age := range ages {
		sum += age
		keys += len(name)
	}
	println(sum, keys)

	squares := make(map[int]int, 100)
	for i := 0; i < 1000; i++ {
		squares[i] = i * i
	}
	for i := 0; i < 1000; i += 2 {
		delete(squares, i)
	}
	total := 0
	for k, v := range squares {
		if k*k != v {
			println("wrong", k, v)
		}
		total += k
	}
	println(len(squares), total, squares[999], squares[998])

	seen := count([]string{"a", "b", "a", "c", "a", "b"})
	println(len(seen), seen["a"], seen["b"], seen["c"])

	grid := map[point]string{{1, 2}: "one-two", {3, 4}: "three-four"}
	grid[point{5, 6}] = "five-six"
	println(len(grid), grid[point{3, 4}], grid[point{9, 9}] == "")

	halves := map[float64]bool{0.5: true, 1.5: false}
	zero := 0.0
	halves[-zero] = true
	println(len(halves), halves[0.5], halves[0], halves[2.5])

	flags := map[bool]uint8{}
	flags[true]++
	flags[true]++
	flags[false]--
	println(flags[true], flags[false])

	for k := range squares {
		if k > 500 {
			break
		}
	}
	first := 0
	for range ages {
		first++
	}
	println(first)
	clear(ages)
	println(len(ages))
	ages["new"] = 1
	println(len(ages), ages["new"])
}
