// exit: 2
package main

func main() {
	seen := map[any]bool{}
	seen[1] = true
	println(len(seen))
	seen[map[string]int{}] = true
}
