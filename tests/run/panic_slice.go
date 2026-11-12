// exit: 2
package main

func cut(s string, from, to int) string { return s[from:to] }

func main() {
	println(cut("abcdef", 2, 4))
	println(cut("abcdef", 4, 2))
}
