// exit: 2
package main

func text(v any) string { return v.(string) }

func main() {
	println(text("x"))
	var nothing any
	println(text(nothing))
}
