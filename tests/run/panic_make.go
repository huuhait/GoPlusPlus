// exit: 2
package main

func room(n int) int { return len(make([]int64, n)) }

func main() {
	println(room(3), room(0))
	println(room(-1))
}
