// exit: 2
package main

var cells []int

func at(i int) int { return cells[i] }

func main() {
	cells = append(cells, 4, 5, 6)
	println(at(2))
	cells = cells[:1]
	println(at(2))
}
