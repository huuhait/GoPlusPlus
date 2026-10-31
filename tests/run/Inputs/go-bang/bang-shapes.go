// The Go form of tests/run/bang-shapes.test (see bang-return.go).
package main

type e1 struct{}

func (e1) Error() string { return "x" }

type big struct {
	a, b, c int
	s       string
}

func fail(n int) (string, []int, map[string]int, [3]int, big, error) {
	if n > 0 {
		return "", nil, nil, [3]int{}, big{}, e1{}
	}
	return "ok", []int{1}, map[string]int{"a": 1}, [3]int{1, 2, 3}, big{1, 2, 3, "z"}, nil
}

func wrap(n int) (string, []int, map[string]int, [3]int, big, error) {
	s, l, m, a, b, err := fail(n)
	if err != nil {
		return "", nil, nil, [3]int{}, big{}, err
	}
	return s, l, m, a, b, nil
}

func wrap2(n int) (string, int, error) {
	s, l, _, _, _, err := fail(n)
	if err != nil {
		return "", 0, err
	}
	return s, len(l), nil
}

func main() {
	s, l, m, a, b, err := wrap(1)
	println(s == "", len(l), len(m), a[2], b.c, err != nil)
	s, l, m, a, b, err = wrap(0)
	println(s, len(l), len(m), a[2], b.s, err != nil)
	s2, n, err := wrap2(1)
	println(s2 == "", n, err != nil)
	s2, n, err = wrap2(0)
	println(s2, n, err != nil)
}
