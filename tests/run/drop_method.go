package main

type File struct{ fd int }

// In a Go file these are methods like any other: nothing calls them
// by itself.
func (f *File) drop() { println("drop", f.fd) }

func (f File) Clone() File {
	println("clone", f.fd)
	return File{f.fd + 1}
}

func main() {
	a := File{1}
	b := a
	b.fd = 5
	{
		c := File{2}
		_ = c
	}
	a.drop()
	println(a.fd, b.fd, b.Clone().fd)
}
