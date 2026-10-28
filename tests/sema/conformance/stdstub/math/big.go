// Declaration-only stub of math/big (test-only).
package big

type Word uint

type form byte

type Float struct {
	prec uint32
	form form
}

func (z *Float) Neg(x *Float) *Float
func (z *Float) Abs(x *Float) *Float
