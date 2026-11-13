package main

type point struct {
	x, y int
}

type small struct {
	a, b int32
}

type named struct {
	name string
	tags []string
}

type celsius float64

type holder struct {
	id    int
	value any
}

func show(v any) {
	switch x := v.(type) {
	case nil:
		println("nil")
	case int:
		println("int", x)
	case int8:
		println("int8", x)
	case uint64:
		println("uint64", x)
	case string:
		println("string", x, len(x))
	case bool:
		println("bool", x)
	case float64:
		println("float64", x)
	case celsius:
		println("celsius", float64(x))
	case small:
		println("small", x.a, x.b)
	case point:
		println("point", x.x, x.y)
	case named:
		println("named", x.name, len(x.tags))
	case []int:
		println("[]int", len(x))
	case [3]string:
		println("[3]string", x[0], x[2])
	case map[string]int:
		println("map", len(x), x["a"])
	case *point:
		println("*point", x.x, x.y)
	case struct{}:
		println("empty")
	case holder:
		println("holder", x.id)
		show(x.value)
	default:
		println("other")
	}
}

// One clause for several types: the variable is the interface value.
func kind(v any) string {
	switch x := v.(type) {
	case int, int8, uint64:
		if n, ok := x.(int); ok {
			return "integer " + string(rune('0'+n%10))
		}
		return "integer"
	case string, []byte:
		return "text"
	case nil:
		return "nothing"
	}
	return "unknown"
}

func pick(n int) any {
	switch n {
	case 0:
		return 7
	case 1:
		return "seven"
	case 2:
		return point{7, 7}
	case 3:
		return []int{7}
	}
	return nil
}

func count(values []any) (ints, texts, rest int) {
	for _, v := range values {
		switch v.(type) {
		case int:
			ints++
		case string:
			texts++
		default:
			rest++
		}
	}
	return
}

func main() {
	var a any
	show(a)
	a = 42
	show(a)
	a = "hello"
	show(a)
	show(true)
	show(1.5)
	show(int8(-3))
	show(uint64(1) << 63)
	show(celsius(36.6))
	show(small{1, -2})
	show(point{1, 2})
	show(named{"n", []string{"a", "b"}})
	show([]int{1, 2, 3})
	show([3]string{"x", "y", "z"})
	show(map[string]int{"a": 1})
	q := point{7, 8}
	show(&q)
	show(struct{}{})
	show(holder{1, holder{2, "inner"}})
	show(uint8(3))
	show(any(5))

	n, ok := a.(int)
	println(n, ok)
	s, ok := a.(string)
	println(s, ok)
	if p, ok := pick(2).(point); ok {
		println(p.x + p.y)
	}
	println(pick(0).(int)+1, pick(1).(string)+"!", len(pick(3).([]int)))
	println(pick(4) == nil, pick(0) == nil)
	println(kind(3), kind(int8(3)), kind("x"), kind(nil), kind(1.5))

	b := a
	println(a == b, a != nil, a == nil)
	a = nil
	println(a == nil, b == nil, b.(string))
	var c any = point{3, 4}
	var d any = point{3, 4}
	println(c == d, c == b, c != d)
	var e any = 3
	var f any = int8(3)
	println(e == f, e == any(3), f == any(int8(3)))
	var g any = 1.5
	println(g == any(1.5), g == any(celsius(1.5)))
	var h any = "ab"
	println(h == any("a"+"b"), h == any("abc"))
	println(any(&q) == any(&q), any(struct{}{}) == any(struct{}{}))
	println(any(small{1, 2}) == any(small{1, 2}), any(small{1, 2}) == any(small{2, 1}))

	values := []any{1, "two", 3.0, 4, nil, "five"}
	values = append(values, point{6, 6}, 7)
	ints, texts, rest := count(values)
	println(len(values), ints, texts, rest)
	total := 0
	for i := 0; i < 1000; i++ {
		var v any = i
		if i%3 == 0 {
			v = "fizz"
		}
		if x, ok := v.(int); ok {
			total += x
		}
	}
	println(total)

	byName := map[string]any{"int": 1, "text": "t", "list": []int{1, 2}}
	byName["point"] = point{1, 1}
	show(byName["int"])
	show(byName["list"])
	show(byName["none"])
	delete(byName, "text")
	println(len(byName))

	seen := map[any]int{}
	seen[1]++
	seen["one"]++
	seen[1]++
	seen[point{1, 1}] = 5
	seen[int8(1)] = 9
	println(len(seen), seen[1], seen["one"], seen[point{1, 1}], seen[int8(1)], seen[2])

	held := holder{id: 9}
	println(held.value == nil)
	held.value = held.id
	copyOf := held
	copyOf.value = "changed"
	show(held.value)
	show(copyOf.value)
	println(held == holder{9, 9}, held == copyOf)
}
