package main

import (
	"fmt"
	"os"
)

type Stats struct {
	Lines, Words, Bytes int
}

var inputs = []string{"input1.txt", "input2.txt"}

func isSpace(c byte) bool {
	return c == ' ' || c == '\n' || c == '\t' || c == '\r'
}

// nextWord skips spaces, then returns the next word and the rest of s.
func nextWord(s string) (word, rest string) {
	i := 0
	for i < len(s) && isSpace(s[i]) {
		i++
	}
	j := i
	for j < len(s) && !isSpace(s[j]) {
		j++
	}
	return s[i:j], s[j:]
}

func count(name string) (Stats, string, error) {
	data, err := os.ReadFile(name)
	if err != nil {
		return Stats{}, "", err
	}
	text := string(data)
	st := Stats{Bytes: len(data)}
	for i := 0; i < len(text); i++ {
		if text[i] == '\n' {
			st.Lines++
		}
	}
	var best string
	rest := text[:]
	for {
		var w string
		w, rest = nextWord(rest)
		if len(w) == 0 {
			break
		}
		st.Words++
		if len(w) > len(best) {
			best = w
		}
	}
	return st, best, nil
}

func main() {
	var total Stats
	for _, name := range inputs {
		st, longest, err := count(name)
		if err != nil {
			panic(err)
		}
		fmt.Printf("%-10s %d lines, %d words, %d bytes, longest %q\n", name, st.Lines, st.Words, st.Bytes, longest)
		total.Lines += st.Lines
		total.Words += st.Words
		total.Bytes += st.Bytes
	}
	fmt.Printf("%-10s %d lines, %d words, %d bytes\n", "total", total.Lines, total.Words, total.Bytes)
}
