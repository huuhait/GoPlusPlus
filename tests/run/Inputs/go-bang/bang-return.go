// The Go form of tests/run/bang-return.test: every `f!(x)` is
// `v, err := f(x); if err != nil { return zero values, err }`, hoisted in
// the order Go++ evaluates it. `go run` of this file must print what
// golden/bang-return.out holds (Inputs/check-bang-go.py checks it; the tests
// themselves never need Go).
package main

type codeErr struct{ code int }

func (e *codeErr) Error() string { return "code error" }

type listErr []int

func (e listErr) Error() string { return "list error" }

var errs = [4]codeErr{{0}, {1}, {2}, {3}}

func id(n int) (int, error) {
	println("  id", n)
	if n < 0 {
		return 0, &errs[1]
	}
	return n, nil
}

func none(n int) error {
	if n > 1 {
		return &errs[n]
	}
	println("  none", n)
	return nil
}

func two(n int) (int, string, error) {
	if n > 1 {
		return 0, "", &errs[n]
	}
	return n, "two", nil
}

func add(a, b int) int { return a + b }

func show(n int, s string) { println("  show", n, s) }

type pair struct{ a, b int }

type T struct{ base int }

func (t *T) get(n int) (int, error) {
	if n > t.base {
		return 0, &errs[2]
	}
	return n, nil
}

type Loader interface{ Load(n int) (int, error) }

func (t *T) Load(n int) (int, error) {
	v, err := t.get(n)
	if err != nil {
		return 0, err
	}
	return v + 100, nil
}

func goStyle(n int) (int, error) {
	if n > 5 {
		return 0, &errs[3]
	}
	return n, nil
}

func fail(n int) error {
	if n == 0 {
		return nil
	}
	return &errs[n]
}

func typedNil() (int, error) {
	var l listErr
	return 0, l
}

func codeOf(err error) int {
	if err == nil {
		return -1
	}
	c, ok := err.(*codeErr)
	if !ok {
		return -2
	}
	return c.code
}

func args(a, b int) (int, error) {
	x, err := id(a)
	if err != nil {
		return 0, err
	}
	y, err := id(b)
	if err != nil {
		return 0, err
	}
	return add(x, y), nil
}

func binary(a, b, c int) (int, error) {
	x, err := id(a)
	if err != nil {
		return 0, err
	}
	y, err := id(b)
	if err != nil {
		return 0, err
	}
	z, err := id(c)
	if err != nil {
		return 0, err
	}
	return x + y*z, nil
}

func lits(n int) (int, error) {
	a, err := id(n)
	if err != nil {
		return 0, err
	}
	b, err := id(n + 1)
	if err != nil {
		return 0, err
	}
	p := pair{a, b}
	c, err := id(n + 2)
	if err != nil {
		return 0, err
	}
	d, err := id(n + 3)
	if err != nil {
		return 0, err
	}
	s := []int{c, d}
	e0, err := id(0)
	if err != nil {
		return 0, err
	}
	e1, err := id(1)
	if err != nil {
		return 0, err
	}
	e2, err := id(2)
	if err != nil {
		return 0, err
	}
	arr := [3]int{e0, e1, e2}
	ix, err := id(1)
	if err != nil {
		return 0, err
	}
	val, err := id(7)
	if err != nil {
		return 0, err
	}
	arr[ix] = val
	return p.a + p.b + s[0] + s[1] + arr[1], nil
}

func control(n int) (int, error) {
	t := 0
	c, err := id(n)
	if err != nil {
		return 0, err
	}
	if c > 0 {
		t += 1
	}
	i, err := id(0)
	if err != nil {
		return 0, err
	}
	for {
		lim, err := id(2)
		if err != nil {
			return 0, err
		}
		if !(i < lim) {
			break
		}
		t += 10
		i++
	}
	tag, err := id(n)
	if err != nil {
		return 0, err
	}
	switch tag {
	case 1:
		t += 100
	default:
		t += 1000
	}
	last, err := id(n)
	if err != nil {
		return 0, err
	}
	return t + last, nil
}

func conv(n int) (float64, error) {
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	return float64(v) / 2, nil
}

func neg(n int) (int, error) {
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	return -v, nil
}

func nested(n int) (int, error) {
	a, err := id(n)
	if err != nil {
		return 0, err
	}
	b, err := id(a)
	if err != nil {
		return 0, err
	}
	c, err := id(b)
	if err != nil {
		return 0, err
	}
	return c, nil
}

func shortCircuit(n int) (int, error) {
	t := 0
	if n < 0 {
		v, err := id(-5)
		if err != nil {
			return 0, err
		}
		if v > 0 {
			t = 1
		}
	}
	if n >= 0 {
		t += 2
	} else {
		v, err := id(-6)
		if err != nil {
			return 0, err
		}
		if v > 0 {
			t += 2
		}
	}
	return t, nil
}

func multi(n int) (int, error) {
	a, s, err := two(n)
	if err != nil {
		return 0, err
	}
	x, y, err := two(n)
	if err != nil {
		return 0, err
	}
	show(x, y)
	if err := none(n); err != nil {
		return 0, err
	}
	return a + len(s), nil
}

var gt = T{base: 3}

func method(n int) (int, error) {
	a, err := gt.get(n)
	if err != nil {
		return 0, err
	}
	b, err := gt.get(n + 1)
	if err != nil {
		return 0, err
	}
	return a + b, nil
}

func viaIface(n int) (int, error) {
	var l Loader = &gt
	a, err := l.Load(n)
	if err != nil {
		return 0, err
	}
	b, err := l.Load(n + 1)
	if err != nil {
		return 0, err
	}
	return a + b, nil
}

func onGoStyle(n int) (int, error) {
	a, err := goStyle(n)
	if err != nil {
		return 0, err
	}
	if err := failBang(n - 2); err != nil {
		return 0, err
	}
	return a, nil
}

func failBang(n int) error {
	return fail(n)
}

func fwd(n int) (int, error) { return idGo(n) }

func idGo(n int) (int, error) {
	v, err := id(n)
	if err != nil {
		return 0, err
	}
	return v, nil
}

func fwdAll(n int) (int, error) { return goStyle(n) }

func named(n int) (r int, err error) {
	r, err = id(n)
	if err != nil {
		return 0, err
	}
	return r, nil
}

func ends(n int) error {
	if err := none(n); err != nil {
		return err
	}
	return nil
}

func typed() (int, error) {
	v, err := typedNil()
	if err != nil {
		return 0, err
	}
	return v, nil
}

func report(what string, v int, err error) {
	println(what, v, codeOf(err))
}

func main() {
	v, err := args(1, 2)
	report("args ok", v, err)
	v, err = args(1, -1)
	report("args fail on b", v, err)
	v, err = args(-1, 2)
	report("args fail on a", v, err)
	v, err = binary(2, 3, 4)
	report("binary ok", v, err)
	v, err = binary(2, 3, -4)
	report("binary fail on c", v, err)
	v, err = lits(0)
	report("lits ok", v, err)
	v, err = lits(-3)
	report("lits fail first", v, err)
	v, err = control(1)
	report("control ok", v, err)
	v, err = control(-1)
	report("control fail", v, err)
	var f float64
	f, err = conv(3)
	println("conv", f, codeOf(err))
	f, err = conv(-3)
	println("conv fail", f, codeOf(err))
	v, err = neg(4)
	report("neg ok", v, err)
	v, err = nested(2)
	report("nested ok", v, err)
	v, err = nested(-2)
	report("nested fail inner", v, err)
	v, err = shortCircuit(1)
	report("short circuit ok", v, err)
	v, err = shortCircuit(-1)
	report("short circuit fail", v, err)
	v, err = multi(1)
	report("multi ok", v, err)
	v, err = multi(2)
	report("multi fail", v, err)
	v, err = method(1)
	report("method ok", v, err)
	v, err = method(3)
	report("method fail second", v, err)
	v, err = viaIface(1)
	report("iface ok", v, err)
	v, err = viaIface(3)
	report("iface fail", v, err)
	v, err = onGoStyle(2)
	report("go style ok", v, err)
	v, err = onGoStyle(6)
	report("go style fail", v, err)
	v, err = onGoStyle(3)
	report("go style fail on the second", v, err)
	v, err = fwd(5)
	report("fwd", v, err)
	v, err = fwdAll(9)
	report("fwdAll", v, err)
	v, err = named(6)
	report("named", v, err)
	v, err = named(-6)
	report("named fail", v, err)
	err = ends(1)
	report("ends ok", 0, err)
	err = ends(3)
	report("ends fail", 0, err)
	// A non-nil interface that holds a nil pointer is an error.
	v, err = typed()
	println("typed nil", v, err != nil, codeOf(err))
	v, err = typedNil()
	println("typed nil direct", v, err != nil, codeOf(err))
	// A `!` function and its Go form are the same function (ERR-1).
	v, err = idGo(8)
	report("same type", v, err)
	println("done")
}
