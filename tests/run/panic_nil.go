// exit: 2
package main

type node struct {
	value int
	next  *node
}

func last(n *node) int {
	for n.next != nil {
		n = n.next
	}
	return n.next.value
}

func main() {
	tail := node{value: 2}
	head := node{value: 1, next: &tail}
	println(head.next.value)
	println(last(&head))
}
