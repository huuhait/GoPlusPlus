package main

import (
	"errors"
	"fmt"
)

type Kind uint8

const (
	Num Kind = iota
	Add
	Sub
	Mul
	Div
	Neg
)

type Node struct {
	Kind Kind
	Val  int64
	Text string // the source text of a number
	L, R *Node
}

type Parser struct {
	src string
	pos int
}

func (p *Parser) skipSpace() {
	for p.pos < len(p.src) && p.src[p.pos] == ' ' {
		p.pos++
	}
}

func (p *Parser) peek() byte {
	p.skipSpace()
	if p.pos >= len(p.src) {
		return 0
	}
	return p.src[p.pos]
}

func (p *Parser) fail(msg string) error {
	return fmt.Errorf("col %d: %s", p.pos+1, msg)
}

func (p *Parser) node(k Kind, l, r *Node) *Node {
	return &Node{Kind: k, L: l, R: r}
}

// expr = term { ("+" | "-") term }
func (p *Parser) expr() (*Node, error) {
	left, err := p.term()
	if err != nil {
		return nil, err
	}
	for {
		op := p.peek()
		if op != '+' && op != '-' {
			return left, nil
		}
		p.pos++
		right, err := p.term()
		if err != nil {
			return nil, err
		}
		k := Add
		if op == '-' {
			k = Sub
		}
		left = p.node(k, left, right)
	}
}

// term = factor { ("*" | "/") factor }
func (p *Parser) term() (*Node, error) {
	left, err := p.factor()
	if err != nil {
		return nil, err
	}
	for {
		op := p.peek()
		if op != '*' && op != '/' {
			return left, nil
		}
		p.pos++
		right, err := p.factor()
		if err != nil {
			return nil, err
		}
		k := Mul
		if op == '/' {
			k = Div
		}
		left = p.node(k, left, right)
	}
}

// factor = number | "(" expr ")" | "-" factor
func (p *Parser) factor() (*Node, error) {
	c := p.peek()
	switch {
	case c == '-':
		p.pos++
		inner, err := p.factor()
		if err != nil {
			return nil, err
		}
		return p.node(Neg, inner, nil), nil
	case c == '(':
		p.pos++
		n, err := p.expr()
		if err != nil {
			return nil, err
		}
		if p.peek() != ')' {
			return nil, p.fail("expected ')'")
		}
		p.pos++
		return n, nil
	case c >= '0' && c <= '9':
		start := p.pos
		var v int64
		for p.pos < len(p.src) && p.src[p.pos] >= '0' && p.src[p.pos] <= '9' {
			v = v*10 + int64(p.src[p.pos]-'0')
			p.pos++
		}
		n := p.node(Num, nil, nil)
		n.Val = v
		n.Text = p.src[start:p.pos]
		return n, nil
	}
	return nil, p.fail("expected a number")
}

func eval(n *Node) (int64, error) {
	switch n.Kind {
	case Num:
		return n.Val, nil
	case Neg:
		v, err := eval(n.L)
		return -v, err
	}
	a, err := eval(n.L)
	if err != nil {
		return 0, err
	}
	b, err := eval(n.R)
	if err != nil {
		return 0, err
	}
	switch n.Kind {
	case Add:
		return a + b, nil
	case Sub:
		return a - b, nil
	case Mul:
		return a * b, nil
	case Div:
		if b == 0 {
			return 0, errors.New("division by zero")
		}
		return a / b, nil
	}
	panic("unknown node kind")
}

func calc(src string) (int64, error) {
	p := Parser{src: src}
	root, err := p.expr()
	if err != nil {
		return 0, err
	}
	p.skipSpace()
	if p.pos < len(p.src) {
		return 0, fmt.Errorf("col %d: unexpected %q", p.pos+1, p.src[p.pos:p.pos+1])
	}
	return eval(root)
}

func main() {
	v, err := calc("6 * 7")
	if err != nil {
		panic(err)
	}
	fmt.Println("answer:", v)

	exprs := []string{"1 + 2 * 3", "(1 + 2) * 3", "10 / (5 - 5)", "2 * (3 + 4) - -5", "7 +", "8 $ 2", "100 / 7 / 2"}
	for _, src := range exprs {
		v, err := calc(src)
		if err != nil {
			fmt.Printf("%-18s error: %v\n", src, err)
			continue
		}
		fmt.Printf("%-18s = %d\n", src, v)
	}
}
