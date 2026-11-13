package main

type Counter struct{ n int }

func (c *Counter) Show() { println("counter", c.n) }

func (c Counter) Value() { println("value of counter", c.n) }

func show(p *int) { println("value", *p) }

type Pair struct {
	left, right Counter
}

// A deferred call has the address of a variable of the loop, and sees
// what is written to it later; each pass has its own variables.
func loops() {
	for i := 0; i < 3; i++ {
		c := Counter{n: i}
		defer c.Show()
		c.n += 10
		v := i * 2
		defer show(&v)
		v++
		// A method that takes a value has a copy as it is now.
		defer c.Value()
		c.n += 100
	}
}

// A part of a variable of a block, and a variable of a block in a
// block that a statement in an untaken branch would borrow.
func parts(n int) {
	{
		p := Pair{}
		defer p.right.Show()
		defer show(&p.left.n)
		p.left.n, p.right.n = n, n+1
	}
	if n > 0 {
		c := Counter{n: 50}
		if n > 5 {
			defer c.Show()
		}
		c.n++
		println("in the branch", c.n)
	}
}

// A function literal and a statement borrow the same variable.
func both() {
	for i := 0; i < 2; i++ {
		c := Counter{n: i}
		defer func() { c.n *= 2 }()
		defer c.Show()
		defer func() { c.n += 5 }()
	}
}

func main() {
	loops()
	parts(1)
	parts(7)
	both()
}
