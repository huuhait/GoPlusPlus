// want: error
// The same without spaces: `a<=>b` is `a <= > b`.
package main

func main() {
	a, b := 1, 2
	_, _ = a, b
	_ = a<=>b
}
