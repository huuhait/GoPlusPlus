// The mechanical Go form of run/bang-funcvalue.test: `x := f!(a)` is
// `x, err := f(a); if err != nil { return zero values, err }`, in the same
// order; `func!` types are `func(...) (..., error)` (ERR-2).
package main

type codeErr struct{ code int }

func (e *codeErr) Error() string { return "code " + string(rune('0'+e.code)) }

var errs = [5]codeErr{{0}, {1}, {2}, {3}, {4}}

func half(n int) (int, error) {
	if n%2 != 0 {
		return 0, &errs[0]
	}
	return n / 2, nil
}

func dec(n int) (int, error) {
	if n <= 0 {
		return 0, &errs[1]
	}
	return n - 1, nil
}

type acc struct{ limit int }

func (a acc) take(n int) (int, error) {
	if n > a.limit {
		return 0, &errs[2]
	}
	return n, nil
}

func twice(f func(int) (int, error), n int) (int, error) {
	v, err := f(n)
	if err != nil {
		return 0, err
	}
	return v * 2, nil
}

func run(n int) (int, error) {
	h := half
	d := dec
	a := acc{limit: 9}
	t := a.take
	lit := func(x int) (int, error) {
		if x == 2 {
			return 0, &errs[3]
		}
		return x * 10, nil
	}
	check := func() error {
		if n == 6 {
			return &errs[4]
		}
		return nil
	}
	var o func(int) (int, error) = half
	total, err := h(n)
	if err != nil {
		return 0, err
	}
	println("half", total)
	v, err := d(n)
	if err != nil {
		return 0, err
	}
	total += v
	println("dec", total)
	v, err = t(n)
	if err != nil {
		return 0, err
	}
	total += v
	println("take", total)
	v, err = lit(n)
	if err != nil {
		return 0, err
	}
	total += v
	println("lit", total)
	if err := check(); err != nil {
		return 0, err
	}
	if f := o; f != nil {
		v, err := f(n)
		if err != nil {
			return 0, err
		}
		total += v
	}
	v, err = twice(dec, n-4)
	if err != nil {
		return 0, err
	}
	total += v
	return total, nil
}

func nested(n int) (int, error) {
	via := func(x int) (int, error) {
		v, err := dec(x)
		if err != nil {
			return 0, err
		}
		return v + 1, nil
	}
	v, err := via(n)
	if err != nil {
		return 0, err
	}
	return v * 10, nil
}

func main() {
	for _, n := range []int{3, 0} {
		v, err := nested(n)
		println("nested", n, v, err != nil)
	}
	for _, n := range []int{8, 1, 0, 10, 2, 6, 4} {
		v, err := run(n)
		if err != nil {
			println(n, "failed:", err.Error())
		} else {
			println(n, "=", v)
		}
	}
}
