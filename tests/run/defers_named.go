package main

func double(p *int) {
	*p *= 2
	println("double", *p)
}

func rename(p *string, to string) {
	println("rename", *p, to)
	*p = to
}

func grow(p *[]int) {
	*p = append(*p, len(*p))
	println("grow", len(*p))
}

// The values of a return are put into the results first; a deferred
// call that changes a result changes what is returned.
func values(n int) (total int, name string) {
	defer double(&total)
	defer rename(&name, "renamed")
	if n == 0 {
		return
	}
	total = n
	if n == 1 {
		name = "one"
		return
	}
	return n * 10, "many"
}

func list(n int) (out []int, err string) {
	defer grow(&out)
	out = append(out, 7)
	if n > 0 {
		defer println("deferred print", n, len(out), err)
		return []int{1, 2, 3}, "full"
	}
	return
}

func note(p *int) { *p++ }

// A result without a name keeps the value of the return.
func parse(n int) (_ int, count int) {
	defer note(&count)
	if n > 0 {
		return n * 2, n
	}
	return 7, 0
}

// A way out of a loop over a value that the loop keeps.
func walk(n int) int {
	total := 0
	defer note(&total)
	for _, x := range make([]int, n) {
		if x > 0 {
			return total
		}
		total++
	}
	return total
}

func main() {
	p, q := parse(3)
	r, t := parse(0)
	println(p, q, r, t, walk(2))
	for i := 0; i < 3; i++ {
		t, s := values(i)
		println(t, s)
	}
	a, e := list(0)
	println(len(a), a[0], e)
	b, f := list(1)
	println(len(b), b[3], f)
	defer println("main is left", 1, true)
	println("end of main")
}
