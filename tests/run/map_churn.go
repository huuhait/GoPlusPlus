package main

const keys = 2048

// A map against an array that holds the same: many keys come and go,
// so the table grows, keeps slots of deleted keys and is laid out anew.
func main() {
	var held [keys]uint32
	var there [keys]bool
	table := map[uint32]uint32{}
	names := map[string]uint32{}
	count := 0
	state := uint32(12345)
	for step := 0; step < 60000; step++ {
		state = state*1664525 + 1013904223
		key := (state >> 8) % keys
		name := string(rune('a'+key%26)) + string(rune('a'+key/26%26)) + string(rune('a'+key/676))
		switch (state >> 28) % 4 {
		case 0, 1:
			if !there[key] {
				count++
			}
			there[key] = true
			held[key] = state
			table[key] = state
			names[name] = state
		case 2:
			if there[key] {
				count--
			}
			there[key] = false
			held[key] = 0
			delete(table, key)
			delete(names, name)
		default:
			value, ok := table[key]
			if ok != there[key] || value != held[key] || names[name] != held[key] {
				println("wrong at step", step, key, value, ok)
				return
			}
		}
		if len(table) != count || len(names) != count {
			println("wrong length at step", step, len(table), len(names), count)
			return
		}
	}
	walked := 0
	sum := uint32(0)
	for key, value := range table {
		if !there[key] || held[key] != value {
			println("wrong entry", key, value)
		}
		walked++
		sum += value
	}
	named := uint32(0)
	for _, value := range names {
		named += value
	}
	println(count, walked, sum == named)
}
