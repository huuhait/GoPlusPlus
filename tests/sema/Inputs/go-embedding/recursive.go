// want: error
// A struct that embeds itself has infinite size.
package main

type T struct{ T }

type U struct{ *U }

func main() { var u U; _ = u }
