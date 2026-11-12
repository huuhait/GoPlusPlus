package main

func wrap(a int8, b int8) int8 { return a + b }

func least() int { return -9223372036854775808 }

func shift(x int, by uint) int { return x << by }

func main() {
	println("wrap", wrap(127, 1), wrap(-128, -1))
	println("least", least()/-1, least()%-1, -least())
	println("shift", shift(1, 62), shift(1, 63), shift(1, 64), shift(-1, 200))
	var u uint8 = 200
	u += 100
	println("uint8", u, u>>1, ^u)
	println("div", 7/2, -7/2, 7%3, -7%3, 7%-3)
	println("mix", 5&^3, 6|1, 6^3, 1<<3>>1)
}
