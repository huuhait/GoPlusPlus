package p

// Review R12b: go build (1.25) reports N0 and the N1/N2 ring in all 6 orders.
type N0[P any] struct{ f0 *N0[int]; f1 N0[N1[N2[P]]] }
type N1[P any] struct{ f0 P; f1 N2[N1[N0[P]]] }
type N2[P any] struct{ f0 N1[P]; f1 N0[int] }
