// The Go form of tests/run/bang-positions.test (see bang-return.go).
package main

type codeErr struct{ code int }

func (e *codeErr) Error() string { return "code error" }

var errs = [3]codeErr{{0}, {1}, {2}}

func id(n int) (int, error) {
	println("  id", n)
	if n < 0 {
		return 0, &errs[1]
	}
	return n, nil
}

type box struct{ n int }

func mk(n int) (box, error) {
	println("  mk", n)
	if n < 0 {
		return box{}, &errs[2]
	}
	return box{n}, nil
}

func (b box) get() (int, error) {
	println("  get", b.n)
	if b.n > 5 {
		return 0, &errs[2]
	}
	return b.n, nil
}

func codeOf(err error) int {
	if err == nil {
		return -1
	}
	return err.(*codeErr).code
}

func ifInit(n int) (int, error) {
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	if v > 0 {
		return v * 2, nil
	}
	return 0, nil
}

func switchInit(n int) (int, error) {
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	switch v {
	case 1:
		return 10, nil
	}
	return 0, nil
}

func rangeOver(n int) (int, error) {
	t := 0
	c, err := id(n)
	if err != nil {
		return 0, err
	}
	for i := range c {
		t += i
	}
	return t, nil
}

func mapKey(k, w int) (int, error) {
	m := map[int]int{}
	kk, err := id(k)
	if err != nil {
		return 0, err
	}
	ww, err := id(w)
	if err != nil {
		return 0, err
	}
	m[kk] = ww
	k2, err := id(k)
	if err != nil {
		return 0, err
	}
	return m[k2], nil
}

func sliceBound(n int) (int, error) {
	s := []int{1, 2, 3, 4, 5, 6}
	lo, err := id(n)
	if err != nil {
		return 0, err
	}
	hi, err := id(n + 2)
	if err != nil {
		return 0, err
	}
	return len(s[lo:hi]), nil
}

func compound(n int) (int, error) {
	t := 1
	a, err := id(n)
	if err != nil {
		return 0, err
	}
	t += a
	b, err := id(n + 1)
	if err != nil {
		return 0, err
	}
	t *= b
	return t, nil
}

func compare(n int) (bool, error) {
	a, err := id(n)
	if err != nil {
		return false, err
	}
	b, err := id(n + 1)
	if err != nil {
		return false, err
	}
	return a == b, nil
}

func appends(n int) (int, error) {
	var s []int
	a, err := id(n)
	if err != nil {
		return 0, err
	}
	b, err := id(n + 1)
	if err != nil {
		return 0, err
	}
	s = append(s, a, b)
	return len(s), nil
}

type small int8

func convert(n int) (small, error) {
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	return small(v), nil
}

type holder struct{ f int }

func fieldStore(n int) (int, error) {
	var h holder
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	h.f = v
	return h.f, nil
}

func declare(n int) (int, error) {
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	w, err := id(n + 1)
	if err != nil {
		return 0, err
	}
	return v + w, nil
}

func twoResults(n int) (int, int, error) {
	a, err := id(n)
	if err != nil {
		return 0, 0, err
	}
	b, err := id(n + 1)
	if err != nil {
		return 0, 0, err
	}
	return a, b, nil
}

func chain(n int) (int, error) {
	b, err := mk(n)
	if err != nil {
		return 0, err
	}
	v, err := b.get()
	if err != nil {
		return 0, err
	}
	return v, nil
}

func goStyleResult(n int) (int, error) {
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	return v + 1, nil
}

func goStyleBare(n int) (v int, err error) {
	v, err = id(n)
	if err != nil {
		return 0, err
	}
	return v, nil
}

func pair(n int) {
	a, b, err := twoResults(n)
	if err != nil {
		panic(err)
	}
	println("  pair", a, b)
}

func run(what string, v int, err error) {
	println(what, v, codeOf(err))
}

func main() {
	v, err := ifInit(1)
	run("if ok", v, err)
	v, err = ifInit(-1)
	run("if fail", v, err)
	v, err = switchInit(1)
	run("switch ok", v, err)
	v, err = switchInit(-1)
	run("switch fail", v, err)
	v, err = rangeOver(3)
	run("range ok", v, err)
	v, err = rangeOver(-3)
	run("range fail", v, err)
	v, err = mapKey(1, 2)
	run("map ok", v, err)
	v, err = mapKey(-1, 2)
	run("map fail key", v, err)
	v, err = mapKey(1, -2)
	run("map fail value", v, err)
	v, err = sliceBound(1)
	run("slice ok", v, err)
	v, err = sliceBound(-1)
	run("slice fail low", v, err)
	v, err = compound(2)
	run("compound ok", v, err)
	v, err = compound(-1)
	run("compound fail first", v, err)
	b, err := compare(4)
	println("compare", b, codeOf(err))
	b, err = compare(-1)
	println("compare fail", b, codeOf(err))
	v, err = appends(1)
	run("append ok", v, err)
	v, err = appends(-2)
	run("append fail second", v, err)
	var sm small
	sm, err = convert(7)
	println("convert", sm, codeOf(err))
	sm, err = convert(-7)
	println("convert fail", sm, codeOf(err))
	v, err = fieldStore(9)
	run("field ok", v, err)
	v, err = fieldStore(-9)
	run("field fail", v, err)
	v, err = declare(1)
	run("declare ok", v, err)
	v, err = declare(-1)
	run("declare fail", v, err)
	x, y, err := twoResults(1)
	println("two", x, y, codeOf(err))
	x, y, err = twoResults(-1)
	println("two fail", x, y, codeOf(err))
	pair(3)
	v, err = chain(3)
	run("chain ok", v, err)
	v, err = chain(-3)
	run("chain fail mk", v, err)
	v, err = chain(8)
	run("chain fail get", v, err)
	v, err = goStyleResult(1)
	run("go style ok", v, err)
	v, err = goStyleResult(-1)
	run("go style fail", v, err)
	v, err = goStyleBare(5)
	run("go style bare ok", v, err)
	v, err = goStyleBare(-5)
	run("go style bare fail", v, err)
}
