// Declaration-only stub of context (test-only).
package context

type Context interface {
	Done() <-chan struct{}
	Err() error
}

type CancelFunc func()
