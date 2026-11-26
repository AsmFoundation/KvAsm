// Command kvasm-example is KvAsm from Go: it puts, gets and deletes a key, then uploads a 1 MiB
// value in chunks under a long key and downloads a byte range of it, through grpc-go and the
// stubs in kvasmpb, generated from proto/kvasm/v1/kvasm.proto (see generate.go).
//
// Run it with the Go release go.mod names, or a later one:
//
//	go run . [host:port]          (default localhost:50051)
//
// A server to run it against:
//
//	docker run --rm -p 50051:50051 ghcr.io/asmfoundation/kvasm:0.3.0
//
// Exit codes: 0 every call answered as the contract says; 1 one did not (each difference is
// printed); 2 no server answered within 10 seconds. `go run` prints a non-zero code as "exit
// status N" and exits 1 itself; a built binary exits with the code.
package main

import (
	"bytes"
	"context"
	"crypto/sha512"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/connectivity"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	pb "github.com/AsmFoundation/KvAsm/examples/go/kvasmpb"
)

// castagnoli is the table for CRC-32C (Castagnoli), the checksum KvAsm stores; crc32.ChecksumIEEE
// would compute the other CRC-32, which the server rejects. Go uses the CPU's CRC-32C
// instructions (SSE4.2, ARMv8 CRC) for this table when it has them.
var castagnoli = crc32.MakeTable(crc32.Castagnoli)

// wireKey returns the bytes to send in a request's key field for key: keys of 64 bytes or less
// unchanged, longer ones as the 65 bytes 0xFF followed by their SHA-512 (docs/PROTOCOL.md, "Long
// keys").
//
// Every client of one server must send long keys this way, as the .NET client does by default,
// or one logical key is stored under two different keys. Raw and compacted keys never collide:
// they never have the same length. An empty key passes unchanged, and the server refuses it
// with InvalidArgument.
func wireKey(key []byte) []byte {
	if len(key) <= 64 {
		return key
	}
	digest := sha512.Sum512(key)
	return append([]byte{0xFF}, digest[:]...)
}

func main() {
	os.Exit(run(os.Args))
}

// run makes every call once and checks each answer against the contract in kvasm.proto.
//
// args is the command line; args[1], when present, is the server's host:port. It returns the
// exit code the package comment lists. It panics only if the CRC-32C table is wrong, which
// would make every checksum below wrong too.
func run(args []string) int {
	target := "localhost:50051"
	if len(args) > 1 {
		target = args[1]
	}
	if got := crc32.Checksum([]byte("123456789"), castagnoli); got != 0xE3069283 {
		panic(fmt.Sprintf("CRC-32C of \"123456789\" is %#x, not 0xe3069283", got))
	}

	// NewClient connects lazily, so a wrong address only shows up at the first call; waitReady
	// turns "no server" into exit 2 before any call is made. NewClient itself fails only on a
	// target it cannot parse.
	conn, err := grpc.NewClient(target, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s: %v\n", target, err)
		return 2
	}
	defer conn.Close()
	if !waitReady(conn, 10*time.Second) {
		fmt.Fprintf(os.Stderr, "No KvAsm server answers at %s\n", target)
		return 2
	}

	r := &report{}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := runCalls(ctx, pb.NewKvAsmClient(conn), r); err != nil {
		// A call the server refused (a CRC-32C it didn't match, a key over the limit) or a
		// connection that dropped: the status says which.
		s := status.Convert(err)
		r.failures = append(r.failures, fmt.Sprintf("%s: %s", s.Code(), s.Message()))
	}
	for _, failure := range r.failures {
		fmt.Fprintln(os.Stderr, "FAILED "+failure)
	}
	if len(r.failures) > 0 {
		return 1
	}
	return 0
}

// waitReady starts conn connecting and waits until it is ready, for at most timeout.
//
// It returns false when the timeout passes first: nothing that speaks gRPC answers at the
// address. A connection attempt that fails leaves the channel Idle, which needs another
// Connect; the loop gives it one, so a server that starts during the wait is still found.
func waitReady(conn *grpc.ClientConn, timeout time.Duration) bool {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	for {
		state := conn.GetState()
		switch state {
		case connectivity.Ready:
			return true
		case connectivity.Idle:
			conn.Connect()
		}
		if !conn.WaitForStateChange(ctx, state) {
			return false
		}
	}
}

// runCalls makes the calls in the order the package comment gives, recording each answer in r.
//
// ctx bounds every call; kv is the client on a ready connection. It returns the error of the
// first call that ends with a non-OK status (the calls after it are not made, so the keys it
// wrote may stay on the server until the next run deletes them), or nil.
func runCalls(ctx context.Context, kv pb.KvAsmClient, r *report) error {
	// Unary calls. Delete first, so the run starts from a missing key whatever ran before.
	key := wireKey([]byte("example:go"))
	if _, err := kv.Delete(ctx, &pb.DeleteRequest{Key: key}); err != nil {
		return err
	}
	put, err := kv.Put(ctx, &pb.PutRequest{Key: key, Value: []byte("Ada")})
	if err != nil {
		return err
	}
	check(r, "Put created", put.GetCreated(), true)
	got, err := kv.Get(ctx, &pb.GetRequest{Key: key})
	if err != nil {
		return err
	}
	check(r, "Get found", got.GetFound(), true)
	checkBytes(r, "Get value", got.GetValue(), []byte("Ada"))
	deleted, err := kv.Delete(ctx, &pb.DeleteRequest{Key: key})
	if err != nil {
		return err
	}
	check(r, "Delete deleted", deleted.GetDeleted(), true)
	if got, err = kv.Get(ctx, &pb.GetRequest{Key: key}); err != nil {
		return err
	}
	check(r, "Get after Delete found", got.GetFound(), false)

	// Upload under a 66-byte key, which wireKey compacts to 65 bytes. The first message carries
	// the key and total_size, the next ones the data (64 KiB each), and the last the CRC-32C,
	// which the server checks before storing anything. The value is the Python example's, so
	// both print the same checksum.
	bigKey := wireKey([]byte("example:go:media/2026/11/26/a-file-name-long-enough-to-compact.bin"))
	check(r, "Long key sent as", len(bigKey), 65)
	value := make([]byte, 1<<20)
	for i := range value {
		value[i] = byte(i * 31 % 251)
	}
	valueCRC := crc32.Checksum(value, castagnoli)
	messages := []*pb.UploadRequest{{Key: bigKey, TotalSize: proto.Uint64(uint64(len(value)))}}
	for offset := 0; offset < len(value); offset += 64 << 10 {
		messages = append(messages, &pb.UploadRequest{Data: value[offset:min(offset+64<<10, len(value))]})
	}
	messages = append(messages, &pb.UploadRequest{Crc32C: proto.Uint32(valueCRC)})
	upload, err := kv.Upload(ctx)
	if err != nil {
		return err
	}
	for _, message := range messages {
		// io.EOF means the server already ended the call (it refused the upload part-way);
		// CloseAndRecv below returns the status it ended it with.
		if err := upload.Send(message); err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			return err
		}
	}
	uploaded, err := upload.CloseAndRecv()
	if err != nil {
		return err
	}
	check(r, "Upload crc32c", uploaded.GetCrc32C(), valueCRC)

	// Download bytes 1000..1999 in 256-byte chunks: the first message carries the whole value's
	// size, the last the CRC-32C of exactly the range. A client checks the bytes it received
	// against that checksum (a stream that ends without one is not a value); the example also
	// checks them against the bytes it uploaded. An absent field reads as 0, which the checks
	// below report.
	download, err := kv.Download(ctx, &pb.DownloadRequest{Key: bigKey, Offset: 1000, Length: proto.Uint64(1000), ChunkSize: 256})
	if err != nil {
		return err
	}
	var data []byte
	var totalSize uint64
	var rangeCRC uint32
	for {
		message, err := download.Recv()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return err
		}
		if message.TotalSize != nil {
			totalSize = message.GetTotalSize()
		}
		data = append(data, message.GetData()...)
		if message.Crc32C != nil {
			rangeCRC = message.GetCrc32C()
		}
	}
	check(r, "Download crc32c", rangeCRC, crc32.Checksum(data, castagnoli))
	checkBytes(r, "Download bytes", data, value[1000:2000])
	check(r, "Download total_size", totalSize, uint64(len(value)))
	if deleted, err = kv.Delete(ctx, &pb.DeleteRequest{Key: bigKey}); err != nil {
		return err
	}
	check(r, "Delete big deleted", deleted.GetDeleted(), true)
	return nil
}

// report collects the answers that differ from the contract, so a run reports every difference
// instead of stopping at the first.
type report struct {
	failures []string
}

// check prints one answer and records it in r when it differs from want.
//
// what names the call and the field as printed; got is the server's answer; want is what the
// contract says it must be.
func check[T comparable](r *report, what string, got, want T) {
	fmt.Printf("%s: %v\n", what, got)
	if got != want {
		r.failures = append(r.failures, fmt.Sprintf("%s: got %v, expected %v", what, got, want))
	}
}

// checkBytes is check for byte slices, which are not comparable: it prints a value over 16 bytes
// as its length, and a difference as both lengths instead of the bytes themselves.
//
// what names the call and the field as printed; got is the server's answer; want is what the
// contract says it must be.
func checkBytes(r *report, what string, got, want []byte) {
	if len(got) > 16 {
		fmt.Printf("%s: %d bytes\n", what, len(got))
	} else {
		fmt.Printf("%s: %q\n", what, got)
	}
	if !bytes.Equal(got, want) {
		r.failures = append(r.failures, fmt.Sprintf("%s: the %d bytes received differ from the %d expected", what, len(got), len(want)))
	}
}
