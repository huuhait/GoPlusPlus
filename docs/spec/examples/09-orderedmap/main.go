package main

import (
	"cmp"
	"fmt"
)

// OrderedMap keeps its keys sorted. It is a sorted slice of keys plus a
// slice of values at the same positions.
type OrderedMap[K cmp.Ordered, V any] struct {
	keys []K
	vals []V
}

// search returns the position of k, or where it would be inserted.
func (m *OrderedMap[K, V]) search(k K) (int, bool) {
	lo, hi := 0, len(m.keys)
	for lo < hi {
		mid := lo + (hi-lo)/2
		if cmp.Less(m.keys[mid], k) {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	return lo, lo < len(m.keys) && m.keys[lo] == k
}

func (m *OrderedMap[K, V]) Put(k K, v V) {
	i, found := m.search(k)
	if found {
		m.vals[i] = v
		return
	}
	m.keys = append(m.keys, k)
	m.vals = append(m.vals, v)
	for j := len(m.keys) - 1; j > i; j-- {
		m.keys[j], m.keys[j-1] = m.keys[j-1], m.keys[j]
		m.vals[j], m.vals[j-1] = m.vals[j-1], m.vals[j]
	}
}

// Get returns a pointer to the value, or nil if k is absent.
func (m *OrderedMap[K, V]) Get(k K) *V {
	i, found := m.search(k)
	if !found {
		return nil
	}
	return &m.vals[i]
}

func (m *OrderedMap[K, V]) Len() int { return len(m.keys) }

func (m *OrderedMap[K, V]) Each(f func(k K, v V)) {
	for i := range m.keys {
		f(m.keys[i], m.vals[i])
	}
}

func main() {
	words := []string{"the", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog", "the", "fox"}

	var counts OrderedMap[string, int]
	for _, w := range words {
		if p := counts.Get(w); p != nil {
			*p++
		} else {
			counts.Put(w, 1)
		}
	}
	fmt.Println("distinct words:", counts.Len())
	counts.Each(func(k string, v int) {
		fmt.Printf("%-6s %d\n", k, v)
	})
	if p := counts.Get("fox"); p != nil {
		fmt.Println("fox =", *p)
	}
	if counts.Get("cat") == nil {
		fmt.Println("cat is absent")
	}

	var colors OrderedMap[string, []string]
	colors.Put("pear", []string{"green", "yellow"})
	colors.Put("apple", []string{"red", "green"})
	colors.Put("plum", []string{"purple"})
	colors.Put("apple", []string{"red"}) // replaces the old value
	colors.Each(func(k string, v []string) {
		fmt.Println(k, v)
	})
}
