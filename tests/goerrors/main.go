// Command goerrors compares the errors that gopp reports for the test files
// of Go's type checker with the errors that those files expect, by the rules
// of Go's own harness (go/types/check_test.go):
//
// A comment /* ERROR "text" */ or /* ERRORx "regexp" */ (or the same with
// //) right after a token expects an error at that token whose message
// contains the text or matches the regexp. Several such comments in a row
// expect several errors at the token.
//
// It reads from standard input, fields separated by tabs:
//
//	file <path>                          a file that was checked
//	err <path> <line> <column> <message> an error reported in a checked file
//
// and prints:
//
//	ok <count>                                  errors that were expected
//	missing <path>:<line>:<col>: <pattern>      expected, not reported
//	unexpected <path>:<line>:<col>: <message>   reported, not expected
//	column <path>:<line>:<col>: <message>       expected in another column
package main

import (
	"bufio"
	"fmt"
	"go/scanner"
	"go/token"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

type comment struct {
	line, col int
	text      string
}

var expects = regexp.MustCompile("^ ERRORx? ")

// commentMap is the function of that name in go/types/commentMap_test.go:
// for each line, the expectations at the tokens of that line.
func commentMap(src []byte) map[int][]comment {
	fset := token.NewFileSet()
	file := fset.AddFile("", -1, len(src))
	var s scanner.Scanner
	s.Init(file, src, nil, scanner.ScanComments)
	res := map[int][]comment{}
	var prev token.Pos
	for {
		pos, tok, lit := s.Scan()
		switch tok {
		case token.EOF:
			return res
		case token.COMMENT:
			if lit[1] == '*' {
				lit = lit[:len(lit)-2]
			}
			lit = lit[2:]
			if expects.MatchString(lit) {
				p := fset.Position(prev)
				res[p.Line] = append(res[p.Line], comment{p.Line, p.Column, lit})
			}
		case token.SEMICOLON:
			if lit == "\n" {
				continue
			}
			fallthrough
		default:
			prev = pos
		}
	}
}

var (
	uint8Name = regexp.MustCompile(`\buint8\b`)
	int32Name = regexp.MustCompile(`\bint32\b`)
)

// matches reports whether the message is the one that the comment expects.
// Go names a type byte or rune where the source does; gopp has one name
// for byte and uint8 and one for rune and int32, so a message also matches
// with those names in the place of uint8 and int32.
func matches(want comment, message string) bool {
	as := func(byteName, runeName string) string {
		message := uint8Name.ReplaceAllString(message, byteName)
		return int32Name.ReplaceAllString(message, runeName)
	}
	return matchesText(want, message) || matchesText(want, as("byte", "int32")) ||
		matchesText(want, as("uint8", "rune")) || matchesText(want, as("byte", "rune"))
}

func matchesText(want comment, message string) bool {
	pattern, substr := strings.CutPrefix(want.text, " ERROR ")
	if !substr {
		pattern = strings.TrimPrefix(want.text, " ERRORx ")
	}
	unquoted, err := strconv.Unquote(strings.TrimSpace(pattern))
	check(err)
	if substr {
		return strings.Contains(message, unquoted)
	}
	return regexp.MustCompile(unquoted).MatchString(message)
}

type reported struct {
	line, col int
	message   string
}

func main() {
	var paths []string
	errs := map[string][]reported{}
	in := bufio.NewScanner(os.Stdin)
	in.Buffer(nil, 1<<24)
	for in.Scan() {
		fields := strings.SplitN(in.Text(), "\t", 5)
		switch fields[0] {
		case "file":
			paths = append(paths, fields[1])
		case "err":
			line, err := strconv.Atoi(fields[2])
			check(err)
			col, err := strconv.Atoi(fields[3])
			check(err)
			errs[fields[1]] = append(errs[fields[1]], reported{line, col, fields[4]})
		}
	}
	check(in.Err())

	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	ok := 0
	for _, path := range paths {
		src, err := os.ReadFile(path)
		check(err)
		expected := commentMap(src)
		for _, got := range errs[path] {
			list := expected[got.line]
			index, delta := -1, 0
			for i, want := range list {
				if !matches(want, got.message) {
					continue
				}
				d := want.col - got.col
				if d < 0 {
					d = -d
				}
				if index < 0 || d < delta {
					index, delta = i, d
				}
			}
			if index < 0 {
				fmt.Fprintf(out, "unexpected %s:%d:%d: %s\n", path, got.line, got.col, got.message)
				continue
			}
			if delta > 0 {
				fmt.Fprintf(out, "column %s:%d:%d: %s\n", path, got.line, list[index].col, got.message)
			} else {
				ok++
			}
			expected[got.line] = append(list[:index:index], list[index+1:]...)
		}
		var lines []int
		for line := range expected {
			lines = append(lines, line)
		}
		sort.Ints(lines)
		for _, line := range lines {
			for _, want := range expected[line] {
				fmt.Fprintf(out, "missing %s:%d:%d:%s\n", path, want.line, want.col, want.text)
			}
		}
	}
	fmt.Fprintf(out, "ok %d\n", ok)
}

func check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
