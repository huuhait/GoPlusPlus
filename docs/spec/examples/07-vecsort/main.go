package main

import (
	"fmt"
	"slices"
)

func main() {
	var v []int32
	for _, x := range []int32{42, 7, 19, 3, 88, 7, 25} {
		v = append(v, x)
	}
	slices.Sort(v)

	out := make([]int32, 0, len(v))
	for _, x := range v {
		out = append(out, x)
	}
	fmt.Println("sorted:", out)

	_, has19 := slices.BinarySearch(v, 19)
	_, has20 := slices.BinarySearch(v, 20)
	fmt.Println("has 19:", has19)
	fmt.Println("has 20:", has20)

	slices.Reverse(v)
	out = out[:0]
	for _, x := range v {
		out = append(out, x)
	}
	fmt.Println("reversed:", out)
}
