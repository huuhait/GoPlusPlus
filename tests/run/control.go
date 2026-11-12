package main

func kind(n int) int {
	switch {
	case n < 0:
		return -1
	case n == 0:
		return 0
	}
	switch n {
	case 1, 2, 3:
		return 1
	case 4:
		fallthrough
	case 5:
		return 5
	default:
		return 9
	}
}

func sum(to int) int {
	total := 0
	for i := 0; i < to; i++ {
		if i%2 == 0 {
			continue
		}
		if i > 7 {
			break
		}
		total += i
	}
	return total
}

func grid() int {
	count := 0
outer:
	for i := 0; i < 4; i++ {
		for j := 0; j < 4; j++ {
			if j == 2 {
				continue outer
			}
			if i == 3 {
				break outer
			}
			count += 10*i + j
		}
	}
	return count
}

func back() int {
	n := 0
again:
	n++
	if n < 5 {
		goto again
	}
	return n
}

func main() {
	println(kind(-4), kind(0), kind(2), kind(4), kind(5), kind(8))
	println(sum(100), grid(), back())
}
