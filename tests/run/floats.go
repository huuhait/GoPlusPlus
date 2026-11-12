package main

func third(x float64) float64 { return x / 3 }

func main() {
	var small float32 = 1.0 / 3
	println(third(1), small, float64(small), third(0)/third(0))
	big := third(3e307)
	println(big*100, -big*100, int(third(8.7)), int(third(-8.7)), float64(7)/2)
	println(third(1) == third(1), third(0) == -third(0), third(0)/third(0) == third(0)/third(0))
}
