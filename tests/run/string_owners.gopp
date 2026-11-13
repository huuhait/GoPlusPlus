package main

type person struct {
	name  string
	words []string
	notes map[string]string
}

type key struct {
	first, last string
	n           int
}

var kept string

func join(parts []string, with string) string {
	out := ""
	for i, part := range parts {
		if i > 0 {
			out += with
		}
		out += part
	}
	return out
}

func kind(name string) string {
	for i := 0; i < 3; i++ {
		switch name + "!" {
		case "a!":
			return "first"
		case "b" + "!":
			break
		default:
			if i == 1 {
				return name[1:] + name[:1]
			}
			continue
		}
		kept = name + name
	}
	return kept
}

func both(text string) (string, string) {
	return text, text + "."
}

func count(text string) int {
	seen := 0
	for i, r := range text {
		if i == 0 {
			text = "replaced " + string(r)
		}
		seen++
	}
	return seen + len(text)
}

func main() {
	hello := "hel" + "lo"
	who := string([]byte{'a', 'n', 'n'})
	line := hello + ", " + who + "!"
	println(line, len(line), line[7:10], line[:5] == hello)
	line += " " + line[:2]
	copyOf := line
	line += "?"
	println(copyOf, line, join([]string{who, "bob", who[1:]}, "-"))
	println(kind("a"), kind("b"), kind("xyz"), kept)
	one, two := both(who)
	_, three := both(one + two)
	println(one, two, three, count("héllo"))

	p := person{name: who, words: []string{hello, who}, notes: map[string]string{who: hello}}
	q := p
	q.name += "a"
	q.words[1] += "e"
	q.words = append(q.words, q.name, string(rune(0x263A)))
	q.notes[who] += "!"
	q.notes[q.name] = q.words[1] + q.notes[who]
	delete(q.notes, "nobody")
	println(p.name, len(p.words))
	println(q.name, q.words[1], q.words[3], q.notes[who], q.notes["anna"], len(q.notes))
	delete(q.notes, who)
	q.words = q.words[1:3]
	q.words[0], q.words[1] = q.words[1], q.words[0]
	println(len(q.notes), q.words[0], q.words[1])

	people := map[key]string{}
	for i := 0; i < 50; i++ {
		k := key{string(rune('a' + i%5)), who + string(rune('0'+i%3)), i % 2}
		people[k] += string(rune('A' + i%26))
	}
	println(len(people), people[key{"a", "ann0", 0}], people[key{"e", "ann2", 1}])
	clear(people)
	people[key{who, who, 1}] = who
	println(len(people), people[key{"ann", "an" + "n", 1}])

	runes := []rune(line)
	bytes := []byte(line[2:6])
	println(string(runes[:3]), string(bytes), string(runes[len(runes)-1]))
}
