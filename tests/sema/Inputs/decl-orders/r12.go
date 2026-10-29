package p

// Review R12: go build (1.25) reports A and B in both orders.
type A struct{ a A; b B[int] }
type B[P any] struct{ b B[P] }
