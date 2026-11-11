// Command gotypes prints what the names of Go's standard library denote, as
// Go's type checker resolves them, so that the name resolution of gopp can
// be compared with it. Run it with CGO_ENABLED=0: the files that import "C"
// are then no part of their packages.
//
// The packages come in dependency order. For each one:
//
//	package <import path> <name>
//	map <path as written in an import> <import path>
//	file <path>
//	def <offset> <kind>
//	use <offset> obj <file index> <offset>      declared in this package
//	use <offset> universe <name>
//	use <offset> ext <import path> <name>       declared in another package
//
// The records of a file follow its line. Only names that a scope holds are
// printed: no fields, no methods, no labels.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"go/types"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
)

type listed struct {
	ImportPath string
	Dir        string
	Name       string
	GoFiles    []string
	ImportMap  map[string]string
}

type importer func(path string) (*types.Package, error)

func (f importer) Import(path string) (*types.Package, error) { return f(path) }

func main() {
	list := exec.Command("go", "list", "-deps", "-json=ImportPath,Dir,Name,GoFiles,ImportMap", "std")
	list.Stderr = os.Stderr
	stream, err := list.StdoutPipe()
	check(err)
	check(list.Start())

	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	checked := map[string]*types.Package{"unsafe": types.Unsafe}
	decoder := json.NewDecoder(stream)
	for {
		var p listed
		if err := decoder.Decode(&p); err == io.EOF {
			break
		} else {
			check(err)
		}
		fmt.Fprintf(out, "package %s %s\n", p.ImportPath, p.Name)
		written := make([]string, 0, len(p.ImportMap))
		for path := range p.ImportMap {
			written = append(written, path)
		}
		sort.Strings(written)
		for _, path := range written {
			fmt.Fprintf(out, "map %s %s\n", path, p.ImportMap[path])
		}
		// Package unsafe is built into the type checker; its file only
		// documents it.
		if p.ImportPath == "unsafe" {
			for _, name := range p.GoFiles {
				fmt.Fprintf(out, "file %s\n", filepath.Join(p.Dir, name))
			}
			fmt.Fprintln(out, "builtin")
			continue
		}

		fset := token.NewFileSet()
		var files []*ast.File
		index := map[string]int{}
		for i, name := range p.GoFiles {
			path := filepath.Join(p.Dir, name)
			file, err := parser.ParseFile(fset, path, nil, parser.SkipObjectResolution)
			check(err)
			files = append(files, file)
			index[path] = i
		}
		config := types.Config{
			Importer: importer(func(path string) (*types.Package, error) {
				if actual, ok := p.ImportMap[path]; ok {
					path = actual
				}
				if pkg := checked[path]; pkg != nil {
					return pkg, nil
				}
				return nil, fmt.Errorf("package %s is not checked yet", path)
			}),
		}
		info := &types.Info{
			Defs: map[*ast.Ident]types.Object{},
			Uses: map[*ast.Ident]types.Object{},
		}
		pkg, err := config.Check(p.ImportPath, fset, files, info)
		check(err)
		checked[p.ImportPath] = pkg

		for i, file := range files {
			fmt.Fprintf(out, "file %s\n", filepath.Join(p.Dir, p.GoFiles[i]))
			offset := func(pos token.Pos) int { return fset.Position(pos).Offset }
			ast.Inspect(file, func(n ast.Node) bool {
				ident, ok := n.(*ast.Ident)
				if !ok {
					return true
				}
				if obj := info.Defs[ident]; scoped(obj) {
					fmt.Fprintf(out, "def %d %s\n", offset(ident.Pos()), kind(obj))
				}
				obj := info.Uses[ident]
				switch {
				case !scoped(obj):
				case obj.Pkg() == nil:
					fmt.Fprintf(out, "use %d universe %s\n", offset(ident.Pos()), obj.Name())
				case obj.Pkg() == pkg:
					at := fset.Position(obj.Pos())
					fmt.Fprintf(out, "use %d obj %d %d\n", offset(ident.Pos()), index[at.Filename], at.Offset)
				default:
					fmt.Fprintf(out, "use %d ext %s %s\n", offset(ident.Pos()), obj.Pkg().Path(), obj.Name())
				}
				return true
			})
		}
	}
	check(list.Wait())
}

// scoped reports whether obj is a name that a scope holds.
func scoped(obj types.Object) bool {
	if obj == nil || obj.Parent() == nil {
		return false
	}
	switch obj.(type) {
	case *types.Label:
		return false
	case *types.Func:
		// A package may have many init functions; none can be named.
		return obj.Name() != "init"
	}
	return true
}

func kind(obj types.Object) string {
	switch obj.(type) {
	case *types.Const:
		return "const"
	case *types.TypeName:
		return "type"
	case *types.Var:
		return "var"
	case *types.Func:
		return "func"
	case *types.PkgName:
		return "import"
	}
	panic(fmt.Sprintf("object %T", obj))
}

func check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
