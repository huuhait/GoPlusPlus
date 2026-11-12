package main

type letters []int32

func main() {
	text := "héllo, 世界 \xff!"
	runes := []rune(text)
	println(len(runes), runes[1], runes[7], runes[10])
	runes[0] = 'J'
	runes = append(runes, 0x1F600, -1, 0xD800)
	back := string(runes)
	println(len(back), back)
	println(len([]rune("")), string([]rune{}) == "", len(string([]rune{65, 0x10FFFF, 0x110000})))
	l := letters("ab")
	println(len(l), l[1], string(l))
	count := 0
	for _, r := range []rune("añ€😀") {
		count += len(string(r))
	}
	println(count)
}
