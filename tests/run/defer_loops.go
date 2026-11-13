package main

func say(what string, n int) { println(what, n) }

// The calls of a loop run after the function, the last first, between
// those of the statements around the loop.
func loops(n int) {
	defer say("before", n)
	for i := 0; i < n; i++ {
		defer say("pass", i)
		if i%2 == 0 {
			defer println("even", i)
		}
	}
	defer say("between", n)
	for i := range n {
		for j := range 2 {
			defer func() { println("inner", i, j) }()
		}
	}
	defer say("after", n)
}

// Each pass has its own variables, which its deferred calls keep.
func kept(names []string) (out string) {
	for _, name := range names {
		prefix := name + ":"
		defer func(at int) {
			out += prefix
			println(prefix, at)
		}(len(out))
		out += "."
	}
	return out
}

// A statement that a jump back runs again.
func jumps(n int) {
	i := 0
again:
	defer say("jump", i)
	i++
	if i < n {
		goto again
	}
	defer say("done", i)
}

// A loop that is left early, and one whose function is left.
func early(n int) int {
	for i := 0; ; i++ {
		defer say("early", i)
		if i == n {
			break
		}
		if i > 5 {
			continue
		}
	}
	for i := 0; i < 10; i++ {
		defer say("late", i)
		if i == 1 {
			return i
		}
	}
	return -1
}

// A panic runs the calls of a loop too, and one of them stops it.
func stopped() (n int) {
	for i := 0; i < 3; i++ {
		defer func() {
			if r := recover(); r != nil {
				n = 100 + i
			}
			println("pass", i, n)
		}()
	}
	var s []int
	return s[3]
}

// No pass at all.
func none() {
	for i := 0; i < 0; i++ {
		defer say("never", i)
	}
	defer say("none", 0)
}

func main() {
	loops(3)
	println(kept([]string{"a", "b", "c"}))
	jumps(3)
	println(early(2))
	println(stopped())
	none()
}
