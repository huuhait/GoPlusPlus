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
//	typ <TAB> start <TAB> end <TAB> type [<TAB> value]
//	                                            each expression that has a
//	                                            value, by its byte range: its
//	                                            type, and its value if it is a
//	                                            constant
//	sel <TAB> start <TAB> end <TAB> kind <TAB> indices <TAB> indirect
//	                                            each selector that reaches a
//	                                            field or a method: field,
//	                                            method or methodexpr, the
//	                                            index of each embedded field
//	                                            on the way and of the member
//	                                            last, and whether a pointer
//	                                            is followed
//	init <name>,<name>                          after the files: the variables
//	                                            that one value initializes, in
//	                                            the order of initialization
//	obj <TAB> name <TAB> kind <TAB> type [<TAB> value]
//	                                            each package-level object: for
//	                                            a declared type the type it was
//	                                            declared as, for a constant its
//	                                            exact value
//
// A type is written in one spelling for all identical types: an interface
// with all its methods in the order of their names, then its type terms.
//
// The records of a file follow its line. Only names that a scope holds are
// printed: no fields, no methods, no labels.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"go/ast"
	"go/constant"
	"go/parser"
	"go/token"
	"go/types"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
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
			Defs:  map[*ast.Ident]types.Object{},
			Uses:  map[*ast.Ident]types.Object{},
			Types: map[ast.Expr]types.TypeAndValue{},
			Selections: map[*ast.SelectorExpr]*types.Selection{},
		}
		pkg, err := config.Check(p.ImportPath, fset, files, info)
		check(err)
		checked[p.ImportPath] = pkg

		for i, file := range files {
			fmt.Fprintf(out, "file %s\n", filepath.Join(p.Dir, p.GoFiles[i]))
			offset := func(pos token.Pos) int { return fset.Position(pos).Offset }
			ast.Inspect(file, func(n ast.Node) bool {
				if expr, ok := n.(ast.Expr); ok {
					if tv := info.Types[expr]; tv.IsValue() && !tv.IsNil() {
						fmt.Fprintf(out, "typ\t%d\t%d\t%s", offset(expr.Pos()), offset(expr.End()), canon(tv.Type))
						if tv.Value != nil {
							fmt.Fprintf(out, "\t%s", exact(tv.Value))
						}
						fmt.Fprintln(out)
					}
				}
				if selector, ok := n.(*ast.SelectorExpr); ok {
					if selection := info.Selections[selector]; selection != nil {
						kind := map[types.SelectionKind]string{
							types.FieldVal: "field", types.MethodVal: "method", types.MethodExpr: "methodexpr",
						}[selection.Kind()]
						var indices []string
						for _, index := range selection.Index() {
							indices = append(indices, strconv.Itoa(index))
						}
						fmt.Fprintf(out, "sel\t%d\t%d\t%s\t%s\t%t\n", offset(selector.Pos()), offset(selector.End()),
							kind, strings.Join(indices, ","), selection.Indirect())
					}
				}
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
		for _, initializer := range info.InitOrder {
			var lhs []string
			for _, v := range initializer.Lhs {
				lhs = append(lhs, v.Name())
			}
			fmt.Fprintf(out, "init %s\n", strings.Join(lhs, ","))
		}
		for _, name := range pkg.Scope().Names() {
			switch obj := pkg.Scope().Lookup(name).(type) {
			case *types.Const:
				fmt.Fprintf(out, "obj\t%s\tconst\t%s\t%s\n", name, canon(obj.Type()), exact(obj.Val()))
			case *types.TypeName:
				if obj.IsAlias() {
					fmt.Fprintf(out, "obj\t%s\talias\t%s\n", name, canon(obj.Type()))
				} else {
					fmt.Fprintf(out, "obj\t%s\ttype\t%s\n", name, canon(obj.Type().Underlying()))
				}
			case *types.Var:
				fmt.Fprintf(out, "obj\t%s\tvar\t%s\n", name, canon(obj.Type()))
			case *types.Func:
				fmt.Fprintf(out, "obj\t%s\tfunc\t%s\n", name, canon(obj.Type()))
			}
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

// exact writes the value of a constant in full: the bytes of a string, so
// that no rule of quoting matters.
func exact(value constant.Value) string {
	if value.Kind() == constant.String {
		return fmt.Sprintf("bytes %x", constant.StringVal(value))
	}
	return value.ExactString()
}

// canon writes a type in the one spelling of gopp for identical types.
func canon(t types.Type) string {
	list := func(n int, at func(int) types.Type) string {
		parts := make([]string, n)
		for i := range parts {
			parts[i] = canon(at(i))
		}
		return strings.Join(parts, ", ")
	}
	switch t := types.Unalias(t).(type) {
	case *types.Basic:
		if t.Kind() == types.UnsafePointer {
			return "unsafe.Pointer"
		}
		// The name of the kind: `byte` is uint8, `rune` is int32.
		return types.Typ[t.Kind()].Name()
	case *types.Named:
		name := t.Obj().Name()
		if pkg := t.Obj().Pkg(); pkg != nil {
			name = pkg.Name() + "." + name
		}
		if args := t.TypeArgs(); args.Len() > 0 {
			name += "[" + list(args.Len(), func(i int) types.Type { return args.At(i) }) + "]"
		}
		return name
	case *types.TypeParam:
		return t.Obj().Name()
	case *types.Pointer:
		return "*" + canon(t.Elem())
	case *types.Slice:
		return "[]" + canon(t.Elem())
	case *types.Array:
		return fmt.Sprintf("[%d]%s", t.Len(), canon(t.Elem()))
	case *types.Map:
		return "map[" + canon(t.Key()) + "]" + canon(t.Elem())
	case *types.Chan:
		switch t.Dir() {
		case types.SendOnly:
			return "chan<- " + canon(t.Elem())
		case types.RecvOnly:
			return "<-chan " + canon(t.Elem())
		}
		return "chan " + canon(t.Elem())
	case *types.Signature:
		return "func" + signature(t)
	case *types.Struct:
		fields := make([]string, t.NumFields())
		for i := range fields {
			field := t.Field(i)
			fields[i] = canon(field.Type())
			if !field.Embedded() {
				fields[i] = field.Name() + " " + fields[i]
			}
			if tag := t.Tag(i); tag != "" {
				fields[i] += " " + strconv.Quote(tag)
			}
		}
		return "struct{" + strings.Join(fields, "; ") + "}"
	case *types.Interface:
		var parts, terms []string
		for i := 0; i < t.NumMethods(); i++ {
			method := t.Method(i)
			parts = append(parts, method.Name()+signature(method.Type().(*types.Signature)))
		}
		sort.Strings(parts)
		typeTerms(t, map[*types.Interface]bool{}, &terms)
		sort.Strings(terms)
		for i, term := range terms {
			if i == 0 || term != terms[i-1] {
				parts = append(parts, term)
			}
		}
		if len(parts) == 0 {
			return "any"
		}
		return "interface{" + strings.Join(parts, "; ") + "}"
	case *types.Tuple:
		return "(" + list(t.Len(), func(i int) types.Type { return t.At(i).Type() }) + ")"
	case *types.Union:
		terms := make([]string, t.Len())
		for i := range terms {
			terms[i] = canon(t.Term(i).Type())
			if t.Term(i).Tilde() {
				terms[i] = "~" + terms[i]
			}
		}
		return strings.Join(terms, " | ")
	}
	panic(fmt.Sprintf("type %T", t))
}

// typeTerms collects what an interface embeds that is no interface.
func typeTerms(t *types.Interface, seen map[*types.Interface]bool, terms *[]string) {
	if seen[t] {
		return
	}
	seen[t] = true
	for i := 0; i < t.NumEmbeddeds(); i++ {
		embedded := types.Unalias(t.EmbeddedType(i))
		if named, ok := embedded.(*types.Named); ok && named.Obj().Pkg() == nil && named.Obj().Name() == "comparable" {
			*terms = append(*terms, "comparable")
		} else if inner, ok := embedded.Underlying().(*types.Interface); ok {
			typeTerms(inner, seen, terms)
		} else {
			*terms = append(*terms, canon(embedded))
		}
	}
}

// signature writes the parameters and results of a function type, without
// their names.
func signature(sig *types.Signature) string {
	params := make([]string, sig.Params().Len())
	for i := range params {
		params[i] = canon(sig.Params().At(i).Type())
		if sig.Variadic() && i == len(params)-1 {
			params[i] = "..." + canon(sig.Params().At(i).Type().(*types.Slice).Elem())
		}
	}
	results := make([]string, sig.Results().Len())
	for i := range results {
		results[i] = canon(sig.Results().At(i).Type())
	}
	out := "(" + strings.Join(params, ", ") + ")"
	switch len(results) {
	case 0:
	case 1:
		out += " " + results[0]
	default:
		out += " (" + strings.Join(results, ", ") + ")"
	}
	return out
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
