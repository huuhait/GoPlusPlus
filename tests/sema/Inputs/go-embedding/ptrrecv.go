// want: error
// A method with a pointer receiver is promoted to *S, not to S.
package main

type T struct{}

func (t *T) M() {}

type S struct{ T }

type I interface{ M() }

func main() {
	var s S
	var i I = s
	var j I = &s
	_, _ = i, j
}
