package main

import (
	"cmp"
	"fmt"
	"slices"
	"sync"
)

type Job struct {
	ID   int
	Text string
}

type Result struct {
	ID   int
	Text string
	Sum  int
}

func work(jobs chan Job, results chan Result) {
	for job := range jobs {
		sum := 0
		for i := 0; i < len(job.Text); i++ {
			sum += int(job.Text[i])
		}
		results <- Result{ID: job.ID, Text: job.Text, Sum: sum}
	}
}

func main() {
	words := []string{"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"}

	jobs := make(chan Job, 4)
	results := make(chan Result, 16)

	var wg sync.WaitGroup
	for w := 0; w < 3; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			work(jobs, results)
		}()
	}
	go func() {
		wg.Wait()
		close(results)
	}()

	for i, w := range words {
		jobs <- Job{ID: i, Text: w}
	}
	close(jobs)

	var all []Result
	for r := range results {
		all = append(all, r)
	}
	slices.SortFunc(all, func(a, b Result) int { return cmp.Compare(a.ID, b.ID) })

	total := 0
	for _, r := range all {
		fmt.Printf("%d %-8s sum=%d\n", r.ID, r.Text, r.Sum)
		total += r.Sum
	}
	fmt.Println("total:", total)
}
