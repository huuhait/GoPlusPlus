// exit: 2
package main

func main() {
	defer println("main leaves")
	defer func() {
		panic("third")
	}()
	defer func() {
		recover()
		panic("second")
	}()
	panic("first")
}
