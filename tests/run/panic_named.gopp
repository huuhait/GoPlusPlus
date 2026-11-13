// exit: 2
package main

type Level int

func check(l Level) {
	defer println("checked")
	if l > 3 {
		panic(l)
	}
}

func main() {
	check(2)
	check(7)
}
