// exit: 2
package main

func quiet() {
	defer func() {
		println("stopped:", recover() != nil)
	}()
	panic(nil)
}

func main() {
	quiet()
	var e error
	panic(e)
}
