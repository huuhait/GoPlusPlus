package main

func main() {
	var a View[string]
	var b Option[int]
	var c Unique[*int]
	var d Shared[*int]
	var e Weak[*int]
	var f Copy
	var g Clone
	h := move
	i := clone
	j := new_unique
	k := new_shared
	l := new_weak
	_, _, _, _, _, _, _, _, _, _, _, _ = a, b, c, d, e, f, g, h, i, j, k, l
}
