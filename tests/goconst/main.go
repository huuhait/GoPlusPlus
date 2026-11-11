// Command goconst evaluates constant expressions with Go's go/constant, so
// that the constant arithmetic of gopp can be compared with it.
//
// Each line of standard input is one expression in postfix order, its
// parts separated by blanks:
//
//	I:<literal>  F:<literal>  M:<literal>  R:<literal>   an integer, float,
//	                                       imaginary or rune literal
//	B:true  B:false  S:<bytes in hexadecimal>            a boolean, a string
//	+ - * / // % & | ^ &^ && ||            binary; // is integer division
//	== != < <= > >=                        comparisons
//	<<N  >>N                               shifts by N
//	neg  pos  not  comp  comp:N            unary; comp:N is ^x of N unsigned bits
//	int  float  complex  real  imag  makeimag
//
// For each line it prints the kind and the exact value, the value as a
// message shows it, and for a number that is not complex the nearest
// float64 and float32 as bits:
//
//	Float 1/3 | 0.333333 | 3fd5555555555555 | 3eaaaaab
//
// or "panic" where go/constant has no answer.
package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"go/constant"
	"go/token"
	"math"
	"os"
	"strconv"
	"strings"
)

var binary = map[string]token.Token{
	"+": token.ADD, "-": token.SUB, "*": token.MUL, "/": token.QUO, "//": token.QUO_ASSIGN,
	"%": token.REM, "&": token.AND, "|": token.OR, "^": token.XOR, "&^": token.AND_NOT,
	"&&": token.LAND, "||": token.LOR,
}

var comparison = map[string]token.Token{
	"==": token.EQL, "!=": token.NEQ, "<": token.LSS, "<=": token.LEQ, ">": token.GTR, ">=": token.GEQ,
}

var literal = map[byte]token.Token{'I': token.INT, 'F': token.FLOAT, 'M': token.IMAG, 'R': token.CHAR}

func main() {
	in := bufio.NewScanner(os.Stdin)
	in.Buffer(nil, 1<<24)
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	for in.Scan() {
		fmt.Fprintln(out, eval(in.Text()))
	}
}

func eval(line string) (result string) {
	defer func() {
		if recover() != nil {
			result = "panic"
		}
	}()
	var stack []constant.Value
	pop := func() constant.Value {
		top := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		return top
	}
	for _, part := range strings.Fields(line) {
		var v constant.Value
		switch {
		case len(part) > 2 && part[1] == ':' && literal[part[0]] != 0:
			v = constant.MakeFromLiteral(part[2:], literal[part[0]], 0)
		case strings.HasPrefix(part, "B:"):
			v = constant.MakeBool(part == "B:true")
		case strings.HasPrefix(part, "S:"):
			bytes, err := hex.DecodeString(part[2:])
			if err != nil {
				panic(err)
			}
			v = constant.MakeString(string(bytes))
		case binary[part] != 0:
			y, x := pop(), pop()
			v = constant.BinaryOp(x, binary[part], y)
		case comparison[part] != 0:
			y, x := pop(), pop()
			v = constant.MakeBool(constant.Compare(x, comparison[part], y))
		case strings.HasPrefix(part, "<<"), strings.HasPrefix(part, ">>"):
			count, err := strconv.ParseUint(part[2:], 10, 32)
			if err != nil {
				panic(err)
			}
			op := token.SHL
			if part[0] == '>' {
				op = token.SHR
			}
			v = constant.Shift(pop(), op, uint(count))
		case strings.HasPrefix(part, "comp:"):
			bits, err := strconv.ParseUint(part[5:], 10, 32)
			if err != nil {
				panic(err)
			}
			v = constant.UnaryOp(token.XOR, pop(), uint(bits))
		default:
			x := pop()
			switch part {
			case "neg":
				v = constant.UnaryOp(token.SUB, x, 0)
			case "pos":
				v = constant.UnaryOp(token.ADD, x, 0)
			case "not":
				v = constant.UnaryOp(token.NOT, x, 0)
			case "comp":
				v = constant.UnaryOp(token.XOR, x, 0)
			case "int":
				v = constant.ToInt(x)
			case "float":
				v = constant.ToFloat(x)
			case "complex":
				v = constant.ToComplex(x)
			case "real":
				v = constant.Real(x)
			case "imag":
				v = constant.Imag(x)
			case "makeimag":
				v = constant.MakeImag(x)
			default:
				panic("unknown part " + part)
			}
		}
		stack = append(stack, v)
	}
	v := pop()
	out := fmt.Sprintf("%s %s | %s", v.Kind(), v.ExactString(), v.String())
	if kind := v.Kind(); kind == constant.Int || kind == constant.Float {
		f64, _ := constant.Float64Val(v)
		f32, _ := constant.Float32Val(v)
		out += fmt.Sprintf(" | %016x | %08x", math.Float64bits(f64), math.Float32bits(f32))
	}
	return out
}
