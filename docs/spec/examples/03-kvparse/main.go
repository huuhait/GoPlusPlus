package main

import (
	"fmt"
	"os"
	"strconv"
)

type Entry struct {
	Key   string
	Value string
	Line  int
}

func isSpace(c byte) bool { return c == ' ' || c == '\t' || c == '\r' }

func trim(s string) string {
	for len(s) > 0 && isSpace(s[0]) {
		s = s[1:]
	}
	for len(s) > 0 && isSpace(s[len(s)-1]) {
		s = s[:len(s)-1]
	}
	return s
}

// cut splits s around the first sep.
func cut(s string, sep byte) (before, after string, found bool) {
	for i := 0; i < len(s); i++ {
		if s[i] == sep {
			return s[:i], s[i+1:], true
		}
	}
	return s, "", false
}

func parse(src string) ([]Entry, error) {
	var out []Entry
	line := 0
	for len(src) > 0 {
		var text string
		text, src, _ = cut(src, '\n')
		line++
		text = trim(text)
		if len(text) == 0 || text[0] == '#' {
			continue
		}
		key, value, ok := cut(text, '=')
		if !ok {
			return nil, fmt.Errorf("line %d: expected key=value", line)
		}
		out = append(out, Entry{Key: trim(key), Value: trim(value), Line: line})
	}
	return out, nil
}

func lookup(entries []Entry, key string) (string, bool) {
	for i := range entries {
		if entries[i].Key == key {
			return entries[i].Value, true
		}
	}
	return "", false
}

func main() {
	data, err := os.ReadFile("config.txt")
	if err != nil {
		panic(err)
	}
	src := string(data)
	entries, err := parse(src)
	if err != nil {
		panic(err)
	}
	for _, e := range entries {
		fmt.Printf("%d: %s = %s\n", e.Line, e.Key, e.Value)
	}
	if v, ok := lookup(entries, "port"); ok {
		port, err := strconv.Atoi(v)
		if err != nil {
			panic(err)
		}
		fmt.Println("port + 1 =", port+1)
	}
	_, err = parse("name=ok\nbroken line\n")
	fmt.Println("error:", err)
}
