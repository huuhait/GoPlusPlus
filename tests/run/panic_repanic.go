// exit: 2
package main

func risky() {
	defer func() {
		if r := recover(); r != nil {
			println("seen, and raised again")
			panic(r)
		}
	}()
	var s []int
	_ = s[3]
}

func main() {
	defer println("main leaves")
	risky()
}
