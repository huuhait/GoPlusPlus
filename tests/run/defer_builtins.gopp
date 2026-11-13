package main

// `recover` as the deferred call itself stops nothing.
func plain() (n int) {
	defer func() {
		if recover() != nil {
			n = 1
		}
	}()
	defer recover()
	panic("plain")
}

// A deferred panic comes when the function is left, with the value
// that its statement had.
func late() (s string) {
	defer func() {
		r := recover()
		s, _ = r.(string)
	}()
	text := "first"
	defer panic(text)
	text = "second"
	return "not this"
}

// The map is the one of the variable when the function is left; the
// key is the one of the statement.
func drop(m map[string]int, key string) int {
	func() {
		defer delete(m, key)
		key = "other"
		m["late"] = 3
	}()
	return len(m)
}

func empty(m map[string]int) int {
	func() {
		defer clear(m)
		m["more"] = 1
	}()
	return len(m)
}

func loops(m map[int]int) int {
	func() {
		for i := 0; i < 3; i++ {
			defer delete(m, i)
		}
	}()
	return len(m)
}

func main() {
	println(plain())
	println(late())
	println(drop(map[string]int{"a": 1, "b": 2}, "a"))
	println(empty(map[string]int{"a": 1}))
	println(loops(map[int]int{0: 0, 1: 1, 2: 2, 3: 3}))
}
