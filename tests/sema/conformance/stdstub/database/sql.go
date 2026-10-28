// Declaration-only stub of database/sql (test-only).
package sql

type Stmt struct {
	closed bool
}

type NamedArg struct {
	Name  string
	Value any
}
