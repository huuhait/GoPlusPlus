package main

type point struct{ x, y int32 }

type person struct {
	name string
	age  uint8
	home point
	tags [2]string
}

type padded struct {
	small int8
	wide  int64
}

type hidden struct {
	a int
	_ int
	b float32
}

type reading struct {
	at    point
	value float64
}

func pick(p point) string {
	switch p {
	case point{0, 0}:
		return "origin"
	case point{1, 0}, point{0, 1}:
		return "unit"
	}
	return "far"
}

func main() {
	a := person{"ann", 31, point{1, 2}, [2]string{"x", "y"}}
	b := a
	println(a == b, a != b, point{1, 2} == a.home)
	b.tags[1] = "z"
	println(a == b, a != b)
	b.tags[1] = "y"
	b.name = "an" + "n"
	println(a == b)

	grid := [2][3]int16{{1, 2, 3}, {4, 5, 6}}
	same := grid
	println(grid == same, [3]int16{4, 5, 6} == grid[1])
	same[1][2]++
	println(grid == same)
	words := [3]string{"a", "b", "c"}
	println(words == [3]string{"a", "b", "c"}, words != [3]string{"a", "b", "d"})
	zero := 0.0
	nan := zero / zero
	floats := [2]float64{zero, nan}
	println([2]float64{zero, 1} == [2]float64{-zero, 1}, floats == floats)
	println(hidden{1, 2, 3} == hidden{1, 9, 3}, hidden{1, 2, 3} == hidden{1, 2, 4})
	println(padded{1, 2} == padded{1, 2}, padded{1, 2} == padded{2, 2})
	println(pick(point{0, 0}), pick(point{0, 1}), pick(point{2, 2}))

	people := map[person]int{a: 1}
	people[b]++
	b.age++
	people[b] = 5
	println(len(people), people[a], people[b], people[person{}])
	delete(people, a)
	println(len(people), people[a])

	seen := map[padded]string{{1, 2}: "one"}
	seen[padded{1, 2}] += "!"
	seen[padded{2, 1}] = "two"
	println(len(seen), seen[padded{1, 2}], seen[padded{2, 1}])

	lines := map[[2]string]int{}
	for i := 0; i < 200; i++ {
		key := [2]string{string(rune('a' + i%26)), string(rune('A' + i%7))}
		lines[key] += i
	}
	println(len(lines), lines[[2]string{"a", "A"}], lines[[2]string{"z", "D"}])

	values := map[reading]int{}
	values[reading{point{1, 1}, zero}] = 1
	values[reading{point{1, 1}, -zero}]++
	values[reading{point{1, 1}, nan}] = 7
	values[reading{point{1, 1}, nan}] = 8
	println(len(values), values[reading{point{1, 1}, 0}], values[reading{point{1, 1}, nan}])

	loose := map[hidden]int{{1, 2, 3}: 1}
	loose[hidden{1, 7, 3}]++
	println(len(loose), loose[hidden{1, 0, 3}])
}
