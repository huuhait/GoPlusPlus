package main

import (
	"fmt"
	"hash/crc32"
	"os"
)

func main() {
	data, err := os.ReadFile("input.txt")
	if err != nil {
		panic(err)
	}
	fmt.Printf("size:    %d bytes\n", len(data))
	fmt.Printf("crc32:   %08x\n", crc32.ChecksumIEEE(data))

	// the same checksum, fed in chunks of 7 bytes
	var crc uint32
	for i := 0; i < len(data); i += 7 {
		chunk := data[i:min(i+7, len(data))]
		crc = crc32.Update(crc, crc32.IEEETable, chunk)
	}
	fmt.Printf("chunked: %08x\n", crc)

	fmt.Printf("check:   %08x\n", crc32.ChecksumIEEE([]byte("123456789")))
}
