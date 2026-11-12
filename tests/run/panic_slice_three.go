// exit: 2
package main

var cells []int

func cut(lo, hi, max int) int { return len(cells[lo:hi:max]) }

func main() {
	cells = make([]int, 4, 4)
	println(cut(1, 3, 4), cut(0, 0, 0), cut(4, 4, 4))
	println(cut(1, 3, 2))
}
