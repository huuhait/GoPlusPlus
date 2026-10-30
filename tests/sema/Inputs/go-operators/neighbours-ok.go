// want: ok
// The tokens next to `<=>` keep their Go meaning: `<=`, `>`, `<-` and `<`.
package main

func main() {
	a, b := 1, 2
	c := make(chan int, 1)
	c <- a
	_ = a <= b && b > a || a < -b
	_ = <-c
	_ = a<=b
}
