package main

func join(a, b string) string { return a + "-" + b }

func first(s string) byte { return s[0] }

func order(a, b string) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

func kind(word string) int {
	switch word {
	case "":
		return 0
	case "one", "uno":
		return 1
	case "two":
		return 2
	}
	return -1
}

func main() {
	s := "héllo, 世界"
	t := join("ab", "cd")
	println(t, len(t), t == "ab-cd", t != "ab-cd", t <= "ab-cd", t >= "ab-ce")
	println(order("", ""), order("", "a"), order("abc", "abd"), order("b", "abc"), order("ab", "abc"))
	println(s[1:3], s[:1], s[7:], len(s[2:2]), len(s[len(s):]), first(s), s[len(s)-1])
	println(kind(""), kind("uno"), kind("two"), kind(t[3:]), kind("on"+"e"))
	u := "x"
	u += "y"
	u += string(rune(0x4e16)) + string(rune(-1)) + string(rune(0xd800)) + string(rune(0x110000))
	println(u, len(u), string(rune(65))+string(rune(0x7ff))+string(rune(0x10ffff)))
	empty := ""
	empty += ""
	println(len(empty+empty), empty == "", join(empty, empty))
	half := s[:8]
	whole := half + s[8:]
	println(whole == s, half < s, s[1:3] == "é")
}
