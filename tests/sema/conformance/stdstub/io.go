// Declaration-only stub of io (test-only).
package io

type Writer interface {
	Write(p []byte) (n int, err error)
}

type Reader interface {
	Read(p []byte) (n int, err error)
}

type ReadWriter interface {
	Reader
	Writer
}

var Discard Writer

type Closer interface {
	Close() error
}

type ReadCloser interface {
	Reader
	Closer
}

type WriteCloser interface {
	Writer
	Closer
}
