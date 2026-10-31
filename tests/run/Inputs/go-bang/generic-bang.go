// The mechanical Go form of run/generic-bang.test (check-bang-go.py): each
// `x := f!(a)` is `x, err := f(a); if err != nil { return zero values, err }`
// in the order Go++ evaluates it, and a `!` in main panics.
package main

type negErr struct{}

func (e *negErr) Error() string { return "negative" }

type bigErr struct{}

func (e *bigErr) Error() string { return "too big" }

type shortErr struct{}

func (e *shortErr) Error() string { return "too short" }

var neg negErr
var big bigErr
var short shortErr

type Num interface{ ~int | ~float64 }

func half[T Num](x T) (T, error) {
	if x < 0 {
		return 0, &neg
	}
	return x / 2, nil
}

func pick[T Num](xs []T, i int) (T, error) {
	if i >= len(xs) {
		return 0, &short
	}
	return xs[i], nil
}

type Acc[T Num] struct{ sum T }

func (a *Acc[T]) add(x T) error {
	if x < 0 {
		return &neg
	}
	a.sum += x
	return nil
}

func (a Acc[T]) get() (T, error) {
	if a.sum > 100 {
		return 0, &big
	}
	return a.sum, nil
}

func total[T Num](xs []T) (T, error) {
	var a Acc[T]
	for _, x := range xs {
		if err := a.add(x); err != nil {
			return 0, err
		}
	}
	g, err := a.get()
	if err != nil {
		return 0, err
	}
	h, err := half(g)
	if err != nil {
		return 0, err
	}
	return h, nil
}

func second[T Num](xs []T) (T, error) {
	p1, err := pick(xs, 1)
	if err != nil {
		return 0, err
	}
	p0, err := pick[T](xs, 0)
	if err != nil {
		return 0, err
	}
	return p1 + p0, nil
}

func Two[T any](n int, x T) (int, T) { return n, x }

func sum(a, b error) (int, error) {
	x, err := Two[error](1, a)
	if err != nil {
		return 0, err
	}
	y, err := Two[error](10, b)
	if err != nil {
		return 0, err
	}
	return x + y, nil
}

func report(name string, err error) {
	if err != nil {
		println(name, "failed:", err.Error())
	} else {
		println(name, "ok")
	}
}

func main() {
	v, err := total([]int{4, 6})
	println(v)
	report("total[int]", err)
	v, err = total([]int{4, -1})
	println(v)
	report("total[int] negative", err)
	f, err := total([]float64{60, 50})
	println(f)
	report("total[float64] large", err)
	f, err = total([]float64{3, 4})
	println(f)
	report("total[float64]", err)
	s, err := second([]int{3, 4})
	println(s)
	report("second[int]", err)
	g, err := second([]float64{1})
	println(g)
	report("second[float64] short", err)
	n, err := sum(nil, nil)
	println(n)
	report("sum", err)
	n, err = sum(nil, &neg)
	println(n)
	report("sum failing", err)
	var e error
	h1, err := half[int](9)
	if err != nil {
		panic(err)
	}
	h2, err := half(2.5)
	if err != nil {
		panic(err)
	}
	t3, err := Two[error](3, e)
	if err != nil {
		panic(err)
	}
	println(h1, h2, t3)
}
