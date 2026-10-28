// Declaration-only stub of the Go package math: just what the conformance
// files use (test-only, see ../README.md).
package math

const (
	Pi                     = 3.14159265358979323846264338327950288419716939937510582097494459
	MaxFloat64             = 0x1p1023 * (1 + (1 - 0x1p-52))
	SmallestNonzeroFloat64 = 0x1p-1022 * 0x1p-52
)

func Sqrt(x float64) float64
func Sin(x float64) float64

func sqrt(x float64) float64
