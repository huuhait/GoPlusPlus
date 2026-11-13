package main

type table struct {
	rows  []int
	names map[string]int
}

func fill(rows []int, names map[string]int) {
	rows[0] = 100
	names["filled"] = 100
}

func made() ([]int, map[string]int) {
	rows := []int{1, 2}
	names := map[string]int{"one": 1}
	return rows, names
}

// A slice and a map are values with one owner, also in a Go file: an
// assignment, a call and a range variable give a copy, and a write to a
// nil map makes the map. Go prints "100 2 2", "7 4", "-1 -1 9" and then
// ends with "assignment to entry in nil map".
func main() {
	rows, names := made()
	fill(rows, names)
	println(rows[0], len(rows), len(names))

	other := rows
	other[0] = 7
	count := names
	count["two"] = 2
	count["three"] = 3
	println(rows[0], len(names))

	grid := [][]int{{1, 2}, {3}}
	for _, row := range grid {
		row[0] = -1
	}
	lists := map[string][]int{"a": {5}}
	for _, list := range lists {
		list[0] = 9
	}
	println(grid[0][0], grid[1][0], lists["a"][0])

	var none map[string]int
	none["made"] = 1
	println(len(none), none == nil)
}
