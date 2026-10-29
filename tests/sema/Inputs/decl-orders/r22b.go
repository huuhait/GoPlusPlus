package p

// Review R22: a ring of right-hand sides through an instance is named where
// Go's walk meets it (N1, the type that comes round again), in all 6 orders
// (go build 1.25).
type N0[P any] N1
type N1 N2[bool]
type N2[P any] = N0[N0[P]]
