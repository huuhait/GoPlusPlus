// exit: 2
package main

type Name string

type Ratio float64

type Flag bool

func show(v any) {
	defer func() {
		r := recover()
		switch r := r.(type) {
		case int:
			println("int", r)
		case string:
			println("string", r)
		case Name:
			println("Name", string(r))
		case error:
			println("error", r.Error())
		case nil:
			println("nil")
		default:
			println("other")
		}
	}()
	panic(v)
}

type failure struct{}

func (failure) Error() string { return "failed" }

func main() {
	show(1)
	show("text")
	show(Name("n"))
	show(failure{})
	show(2.5)
	defer func() {
		panic(Ratio(1.5))
	}()
	defer func() {
		panic(Flag(true))
	}()
	defer func() {
		panic(uint8(200))
	}()
	defer func() {
		panic(-3)
	}()
	defer func() {
		panic(2.5)
	}()
	defer func() {
		panic(false)
	}()
	panic(Name("named"))
}
