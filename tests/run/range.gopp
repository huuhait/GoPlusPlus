package main

func count(n int8) int {
	total := 0
	for i := range n {
		total += int(i)
	}
	return total
}

func main() {
	for i, r := range "héllo, 世界" {
		print(i, ":", r, ":", string(r), " ")
	}
	println()
	for i, r := range "a\xffb\xe2\x82z\xed\xa0\x80\xf0\x9f\x98\xc0\x80\xf4\x90\x80\x80" {
		print(i, ":", r, " ")
	}
	println()
	for i := range 3 {
		print(i, " ")
	}
	println(count(5), count(0), count(-3))
	squares := [4]int{1, 4, 9, 16}
	total := 0
	for i, v := range squares {
		squares[3] = 100
		total += i * v
	}
	println(total, squares[3])
	for i := range squares {
		print(i)
	}
	for range squares {
		print("-")
	}
	println()
	var last int
	var letter rune
	for last, letter = range "abc" {
		if letter == 'b' {
			continue
		}
	}
	println(last, letter)
	grid := [2][3]int{{1, 2, 3}, {4, 5, 6}}
	sum := 0
outer:
	for _, row := range grid {
		for at, cell := range row {
			if cell == 5 {
				break outer
			}
			if at == 1 {
				continue
			}
			sum += cell
		}
	}
	println(sum)
	through := &squares
	for i, v := range through {
		through[i] = v + 1
	}
	println(squares[0], squares[3])
	for i := range "" {
		println("never", i)
	}
}
