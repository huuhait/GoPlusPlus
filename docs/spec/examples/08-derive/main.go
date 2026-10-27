package main

import (
	"encoding/json"
	"fmt"
	"maps"
	"slices"
)

type Config struct {
	Name   string
	Tags   []string
	Limits map[string]int
}

// Go has no derive: Equal is written by hand.
func (x Config) Equal(o Config) bool {
	return x.Name == o.Name && slices.Equal(x.Tags, o.Tags) && maps.Equal(x.Limits, o.Limits)
}

func main() {
	a := Config{Name: "demo", Tags: []string{"a", "b"}, Limits: map[string]int{"cpu": 2, "mem": 512}}
	b := Config{Name: "demo", Tags: []string{"a", "b"}, Limits: map[string]int{"cpu": 2, "mem": 512}}

	if !a.Equal(b) {
		panic("assertion failed: a.Equal(b): configs should be equal")
	}
	fmt.Println("a.Equal(b) =", a.Equal(b))

	b.Tags = append(b.Tags, "z")
	b.Limits["cpu"] = 4
	fmt.Println("a.Equal(b) =", a.Equal(b))

	data, err := json.Marshal(a)
	if err != nil {
		panic(err)
	}
	fmt.Println(string(data))

	tmp, other := 1, 2
	tmp, other = other, tmp
	fmt.Println("tmp, other =", tmp, other)
}
