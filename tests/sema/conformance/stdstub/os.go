// Declaration-only stub of os (test-only).
package os

type File struct {
	fd int
}

func (f *File) Write(b []byte) (n int, err error)
func (f *File) Read(b []byte) (n int, err error)

var Stdout *File
