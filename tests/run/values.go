package main

type point struct{ x, y int }

type box struct {
	at    point
	sizes [3]int8
	next  *box
}

func (p point) sum() int { return p.x + p.y }

func (p *point) move(by int) { p.x += by }

func swap(a, b int) (int, int) { return b, a }

func fill(b *box, with int8) {
	for i := 0; i < len(b.sizes); i++ {
		b.sizes[i] = with * int8(i+1)
	}
}

func main() {
	p := point{1, 2}
	q := p
	q.move(10)
	println(p.sum(), q.sum(), p.x == q.x, p.x == q.y-1)
	var b box
	fill(&b, 3)
	first := b
	b.sizes[0] = 100
	b.next = &first
	println(b.sizes[0], b.sizes[1], b.sizes[2], first.sizes[0], b.next.sizes[2])
	x, y := swap(1, 2)
	x, y = y, x+y
	println(x, y, b.next == &first, b.next.next == nil)
	cells := [4]int{1, 2, 3, 4}
	i := 1
	i, cells[i] = 2, 20
	println(i, cells[0], cells[1], cells[2], len(cells))
}
