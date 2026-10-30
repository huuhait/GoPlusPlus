// want: error
// `<=>` is `<=` then `>` in Go (LEX-13): a syntax error, as for `go build`.
package main

func main() {
	a, b := 1, 2
	_, _ = a, b
	_ = a <=> b
}
