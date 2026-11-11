package main

import "fmt"

const (
	minDepth = 4
	maxDepth = 10
)

type Node struct {
	Left, Right *Node
}

func bottomUp(depth int) *Node {
	if depth <= 0 {
		return &Node{}
	}
	return &Node{Left: bottomUp(depth - 1), Right: bottomUp(depth - 1)}
}

func (n *Node) check() int {
	if n.Left == nil {
		return 1
	}
	return 1 + n.Left.check() + n.Right.check()
}

func main() {
	stretch := bottomUp(maxDepth + 1)
	fmt.Printf("stretch tree of depth %d\t check: %d\n", maxDepth+1, stretch.check())

	long := bottomUp(maxDepth)

	for depth := minDepth; depth <= maxDepth; depth += 2 {
		iterations := 1 << uint(maxDepth-depth+minDepth)
		check := 0
		for i := 0; i < iterations; i++ {
			check += bottomUp(depth).check()
		}
		fmt.Printf("%d\t trees of depth %d\t check: %d\n", iterations, depth, check)
	}

	fmt.Printf("long lived tree of depth %d\t check: %d\n", maxDepth, long.check())
}
