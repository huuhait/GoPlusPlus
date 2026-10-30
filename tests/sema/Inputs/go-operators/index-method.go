// want: error
package main

type M struct{ d []int }

func (m M) [] (i int) int { return m.d[i] }

func main() {}
