package main

// The stubs in kvasmpb are generated from ../../proto/kvasm/v1/kvasm.proto and committed, so
// `go run .` needs no protoc. After the contract changes, regenerate them here with
//
//	go generate
//
// which needs protoc 36.2 on PATH (github.com/protocolbuffers/protobuf/releases/tag/v36.2): each
// generated file records the protoc version, and CI regenerates with that release and fails on
// any difference, so a newer protoc shows up as a stale-stub failure until CI's pin moves too.
// The two plugins are built into .bin (gitignored) at the versions go.mod pins on its tool
// lines, so a protoc-gen-go installed elsewhere on PATH never decides the output.
//
// kvasm.proto has no go_package option (every language shares it), so the M options map it to
// this module's kvasmpb package, and module= strips the module path from the output path.

//go:generate go build -o .bin/ google.golang.org/protobuf/cmd/protoc-gen-go google.golang.org/grpc/cmd/protoc-gen-go-grpc
//go:generate protoc -I../../proto --plugin=protoc-gen-go=.bin/protoc-gen-go --plugin=protoc-gen-go-grpc=.bin/protoc-gen-go-grpc --go_out=. --go_opt=module=github.com/AsmFoundation/KvAsm/examples/go --go_opt=Mkvasm/v1/kvasm.proto=github.com/AsmFoundation/KvAsm/examples/go/kvasmpb --go-grpc_out=. --go-grpc_opt=module=github.com/AsmFoundation/KvAsm/examples/go --go-grpc_opt=Mkvasm/v1/kvasm.proto=github.com/AsmFoundation/KvAsm/examples/go/kvasmpb kvasm/v1/kvasm.proto
