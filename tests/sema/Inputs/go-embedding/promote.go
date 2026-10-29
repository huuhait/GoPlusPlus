// want: ok
// Promoted fields and methods, an embedded pointer, interface satisfaction
// through promotion, a method value and a method expression.
package main

type Animal struct{ Name string }

func (a *Animal) Rename(n string) { a.Name = n }
func (a Animal) Hello() string    { return "hi " + a.Name }

type Owner struct{ Who string }

func (o *Owner) Owned() bool { return o.Who != "" }

type Dog struct {
	Animal
	*Owner
	Age int
}

type Greeter interface{ Hello() string }
type Namer interface {
	Rename(string)
	Owned() bool
}

func main() {
	d := Dog{Animal: Animal{Name: "rex"}, Owner: &Owner{Who: "ann"}}
	d.Rename("max")
	var g Greeter = d
	var n Namer = &d
	h := d.Hello
	f := (*Dog).Rename
	f(&d, "bo")
	println(d.Name, d.Who, d.Age, g.Hello(), n.Owned(), h())
}
