package p

// Review R12c: go build (1.25) reports N3 and the N2/N4/N1 ring in all 24
// orders: the ring closes before the walk reaches N3 in N1's walk.
type N4 struct{ f0 N1[[2]N3] }
type N3 N3
type N2[P, Q any] struct{ f0 Q; f1 N2[[2][2]P,N4] }
type N1[P any] struct{ f0 [2]N2[[]P,[2]P] }
