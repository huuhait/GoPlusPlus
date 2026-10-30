// goshape measures the shapes that decide how much exception-handling code
// Go++ will emit, on real Go code (Go++ code has the same shape; P04.15,
// ADR-0028). For every function body of the given standard-library packages:
// lines, calls, which calls need a landing pad, the panic sites (bounds,
// division, type assertion and nil checks) and the calls that pass or return
// small structs by value.
//
// A call needs a landing pad (an LLVM `invoke`) when the callee may unwind and
// the caller has something to drop at that point: an owning local in scope
// (string, slice, map, chan, interface, func, a pointer made by `&T{}` or
// `new`, or a struct or array holding one; parameters and range values are
// borrowed in Go++), an owning temporary of an enclosing call, or a `defer`.
// A callee "cannot unwind" when it is a body in the measured packages with no
// call (other than len/cap/min/max), no bounds, division or assertion check.
// A panic site is a branch to a panic call, which unwinds, so it is an
// `invoke` under the same rule.
//
//	nix develop -c go run bench/eh/goshape.go strings strconv fmt ...
package main

import (
	"fmt"
	"go/ast"
	"go/build"
	"go/constant"
	"go/importer"
	"go/parser"
	"go/token"
	"go/types"
	"os"
	"sort"
)

type stats struct {
	funcs, lines                      int
	calls, callsMayUnwind, callsPad   int
	builtinCalls                      int
	bounds, divs, asserts, nilChecks  int
	panicsPad                         int
	structArgs, aggResults, errResult int
	deferFuncs                        int
	ownerLocals, liveAtPad            int
	ifs, loops                        int
	sizes                             []int
}

var (
	fset   = token.NewFileSet()
	sizes  = types.SizesFor("gc", "amd64")
	safe   = map[string]bool{} // full names of bodies that cannot unwind
	total  stats
	owners = map[types.Type]bool{}
)

func owning(t types.Type) bool {
	if v, ok := owners[t]; ok {
		return v
	}
	owners[t] = false // recursion guard
	r := false
	switch u := t.Underlying().(type) {
	case *types.Basic:
		r = u.Kind() == types.String
	case *types.Slice, *types.Map, *types.Chan, *types.Interface, *types.Signature:
		r = true
	case *types.Array:
		r = owning(u.Elem())
	case *types.Struct:
		for i := 0; i < u.NumFields(); i++ {
			if owning(u.Field(i).Type()) {
				r = true
			}
		}
	}
	owners[t] = r
	return r
}

func smallStruct(t types.Type) bool {
	_, ok := t.Underlying().(*types.Struct)
	return ok && sizes.Sizeof(t) <= 32
}

func fullName(o types.Object) string {
	f, ok := o.(*types.Func)
	if !ok || f.Pkg() == nil {
		return ""
	}
	return f.FullName()
}

type pkgInfo struct {
	files []*ast.File
	info  *types.Info
}

func load(path string, imp types.Importer) (*pkgInfo, error) {
	bp, err := build.Default.Import(path, "", 0)
	if err != nil {
		return nil, fmt.Errorf("import %s: %w", path, err)
	}
	var files []*ast.File
	for _, name := range bp.GoFiles {
		f, err := parser.ParseFile(fset, bp.Dir+"/"+name, nil, 0)
		if err != nil {
			return nil, fmt.Errorf("parse %s: %w", name, err)
		}
		files = append(files, f)
	}
	info := &types.Info{
		Types: map[ast.Expr]types.TypeAndValue{},
		Defs:  map[*ast.Ident]types.Object{},
		Uses:  map[*ast.Ident]types.Object{},
	}
	conf := types.Config{Importer: imp, Error: func(error) {}}
	if _, err := conf.Check(path, fset, files, info); err != nil {
		return nil, fmt.Errorf("check %s: %w", path, err)
	}
	return &pkgInfo{files, info}, nil
}

// fn walks one function body.
type fn struct {
	info      *pkgInfo
	st        *stats
	scopes    []int // owning locals per open block
	hasDefer  bool
	panicSite bool // the body has a check or a call that can unwind
}

func (f *fn) live() int {
	n := 0
	for _, s := range f.scopes {
		n += s
	}
	if f.hasDefer {
		n++
	}
	return n
}

func (f *fn) typeOf(e ast.Expr) types.Type {
	if tv, ok := f.info.info.Types[e]; ok {
		return tv.Type
	}
	return nil
}

func (f *fn) panic(pending int) {
	f.panicSite = true
	if f.live()+pending > 0 {
		f.st.panicsPad++
	}
}

// producesOwner: an expression whose value is a new owning temporary.
func (f *fn) producesOwner(e ast.Expr) bool {
	switch x := ast.Unparen(e).(type) {
	case *ast.CallExpr, *ast.CompositeLit:
		t := f.typeOf(x)
		return t != nil && owning(t)
	case *ast.BinaryExpr:
		t := f.typeOf(x)
		return x.Op == token.ADD && t != nil && owning(t)
	}
	return false
}

func (f *fn) expr(e ast.Expr, pending int) {
	switch x := e.(type) {
	case nil:
	case *ast.FuncLit:
		analyze(f.info, x.Type, x.Body, f.st)
	case *ast.CallExpr:
		f.call(x, pending)
	case *ast.IndexExpr:
		f.expr(x.X, pending)
		f.expr(x.Index, pending)
		if t := f.typeOf(x.X); t != nil {
			switch t.Underlying().(type) {
			case *types.Slice, *types.Array, *types.Pointer, *types.Basic:
				if tv := f.info.info.Types[x.Index]; tv.Value == nil || isSlice(t) || isString(t) {
					f.st.bounds++
					f.panic(pending)
				}
			}
		}
	case *ast.SliceExpr:
		f.expr(x.X, pending)
		f.expr(x.Low, pending)
		f.expr(x.High, pending)
		f.expr(x.Max, pending)
		f.st.bounds++
		f.panic(pending)
	case *ast.BinaryExpr:
		f.expr(x.X, pending)
		f.expr(x.Y, pending)
		if x.Op == token.QUO || x.Op == token.REM {
			tv := f.info.info.Types[x.Y]
			if t, ok := tv.Type.Underlying().(*types.Basic); ok && t.Info()&types.IsInteger != 0 &&
				(tv.Value == nil || constant.Sign(tv.Value) == 0) {
				f.st.divs++
				f.panic(pending)
			}
		}
	case *ast.TypeAssertExpr:
		f.expr(x.X, pending)
		if x.Type != nil {
			f.st.asserts++
			f.panic(pending)
		}
	default:
		ast.Inspect(e, func(n ast.Node) bool {
			if n == e {
				return true
			}
			if c, ok := n.(ast.Expr); ok {
				f.expr(c, pending)
				return false
			}
			return true
		})
	}
}

func isSlice(t types.Type) bool { _, ok := t.Underlying().(*types.Slice); return ok }
func isString(t types.Type) bool {
	b, ok := t.Underlying().(*types.Basic)
	return ok && b.Info()&types.IsString != 0
}

func (f *fn) call(x *ast.CallExpr, pending int) {
	if tv, ok := f.info.info.Types[x.Fun]; ok && tv.IsType() { // conversion
		for _, a := range x.Args {
			f.expr(a, pending)
		}
		return
	}
	fun := ast.Unparen(x.Fun)
	var obj types.Object
	switch g := fun.(type) {
	case *ast.Ident:
		obj = f.info.info.Uses[g]
	case *ast.SelectorExpr:
		obj = f.info.info.Uses[g.Sel]
		f.expr(g.X, pending)
		if t := f.typeOf(g.X); t != nil && types.IsInterface(t) {
			if n, ok := t.(*types.Named); ok && n.Obj().Name() == "error" && n.Obj().Pkg() == nil {
				f.st.nilChecks++ // Go++: error and any values may be nil
				f.panic(pending)
			}
		}
	case *ast.IndexExpr, *ast.IndexListExpr: // generic instance
		f.expr(g, pending)
	default:
		f.expr(fun, pending)
	}
	temps := 0
	for _, a := range x.Args {
		f.expr(a, pending+temps)
		if f.producesOwner(a) {
			temps++
		}
	}
	if _, ok := obj.(*types.Builtin); ok {
		f.st.builtinCalls++
		return
	}
	f.st.calls++
	mayUnwind := !safe[fullName(obj)]
	if mayUnwind {
		f.panicSite = true
		f.st.callsMayUnwind++
		if l := f.live() + pending + temps; l > 0 {
			f.st.callsPad++
			f.st.liveAtPad += l
		}
	}
	for _, a := range x.Args {
		if t := f.typeOf(a); t != nil && smallStruct(t) {
			f.st.structArgs++
		}
	}
	if t, ok := f.typeOf(x).(*types.Tuple); ok && t.Len() >= 2 {
		f.st.aggResults++
		if types.Identical(t.At(t.Len()-1).Type(), types.Universe.Lookup("error").Type()) {
			f.st.errResult++
		}
	} else if t := f.typeOf(x); t != nil && smallStruct(t) {
		f.st.aggResults++
	}
}

// declOwners counts the owning variables a statement declares.
func (f *fn) declOwners(names []*ast.Ident, values []ast.Expr) int {
	n := 0
	for i, id := range names {
		o := f.info.info.Defs[id]
		if o == nil || id.Name == "_" {
			continue
		}
		t := o.Type()
		if owning(t) {
			n++
		} else if _, ok := t.Underlying().(*types.Pointer); ok && i < len(values) && len(values) == len(names) {
			switch v := ast.Unparen(values[i]).(type) {
			case *ast.UnaryExpr: // &T{...}
				if v.Op == token.AND {
					n++
				}
			case *ast.CallExpr: // new(T)
				if id, ok := v.Fun.(*ast.Ident); ok && id.Name == "new" {
					n++
				}
			}
		}
	}
	return n
}

func (f *fn) block(list []ast.Stmt) {
	f.scopes = append(f.scopes, 0)
	for _, s := range list {
		f.stmt(s)
	}
	f.scopes = f.scopes[:len(f.scopes)-1]
}

func (f *fn) add(n int) {
	f.scopes[len(f.scopes)-1] += n
	f.st.ownerLocals += n
}

func (f *fn) stmt(s ast.Stmt) {
	switch x := s.(type) {
	case nil:
	case *ast.BlockStmt:
		f.block(x.List)
	case *ast.AssignStmt:
		for _, e := range x.Rhs {
			f.expr(e, 0)
		}
		for _, e := range x.Lhs {
			f.expr(e, 0)
		}
		if x.Tok == token.DEFINE {
			var ids []*ast.Ident
			for _, e := range x.Lhs {
				id, _ := e.(*ast.Ident)
				ids = append(ids, id)
			}
			f.add(f.declOwners(ids, x.Rhs))
		}
	case *ast.DeclStmt:
		if g, ok := x.Decl.(*ast.GenDecl); ok {
			for _, sp := range g.Specs {
				if v, ok := sp.(*ast.ValueSpec); ok {
					for _, e := range v.Values {
						f.expr(e, 0)
					}
					f.add(f.declOwners(v.Names, v.Values))
				}
			}
		}
	case *ast.IfStmt:
		f.st.ifs++
		f.scopes = append(f.scopes, 0)
		f.stmt(x.Init)
		f.expr(x.Cond, 0)
		f.stmt(x.Body)
		f.stmt(x.Else)
		f.scopes = f.scopes[:len(f.scopes)-1]
	case *ast.ForStmt:
		f.st.loops++
		f.scopes = append(f.scopes, 0)
		f.stmt(x.Init)
		f.expr(x.Cond, 0)
		f.stmt(x.Post)
		f.stmt(x.Body)
		f.scopes = f.scopes[:len(f.scopes)-1]
	case *ast.RangeStmt: // range values are borrowed in Go++
		f.st.loops++
		f.expr(x.X, 0)
		f.stmt(x.Body)
	case *ast.SwitchStmt:
		f.scopes = append(f.scopes, 0)
		f.stmt(x.Init)
		f.expr(x.Tag, 0)
		for _, c := range x.Body.List {
			cc := c.(*ast.CaseClause)
			for _, e := range cc.List {
				f.expr(e, 0)
			}
			f.block(cc.Body)
		}
		f.scopes = f.scopes[:len(f.scopes)-1]
	case *ast.TypeSwitchStmt:
		f.scopes = append(f.scopes, 0)
		f.stmt(x.Init)
		f.stmt(x.Assign)
		for _, c := range x.Body.List {
			f.block(c.(*ast.CaseClause).Body)
		}
		f.scopes = f.scopes[:len(f.scopes)-1]
	case *ast.SelectStmt:
		for _, c := range x.Body.List {
			cc := c.(*ast.CommClause)
			f.scopes = append(f.scopes, 0)
			f.stmt(cc.Comm)
			for _, b := range cc.Body {
				f.stmt(b)
			}
			f.scopes = f.scopes[:len(f.scopes)-1]
		}
	case *ast.DeferStmt:
		f.hasDefer = true
		f.call(x.Call, 0)
	case *ast.GoStmt:
		f.call(x.Call, 0)
	case *ast.ReturnStmt:
		for _, e := range x.Results {
			f.expr(e, 0)
		}
	case *ast.ExprStmt:
		f.expr(x.X, 0)
	case *ast.SendStmt:
		f.expr(x.Chan, 0)
		f.expr(x.Value, 0)
	case *ast.IncDecStmt:
		f.expr(x.X, 0)
	case *ast.LabeledStmt:
		f.stmt(x.Stmt)
	}
}

func analyze(p *pkgInfo, typ *ast.FuncType, body *ast.BlockStmt, st *stats) bool {
	f := &fn{info: p, st: st}
	f.scopes = []int{0}
	if typ.Results != nil { // named owning results are dropped on unwind
		for _, r := range typ.Results.List {
			if len(r.Names) > 0 {
				f.add(f.declOwners(r.Names, nil))
			}
		}
	}
	ast.Inspect(body, func(n ast.Node) bool {
		if _, ok := n.(*ast.FuncLit); ok {
			return false
		}
		if _, ok := n.(*ast.DeferStmt); ok {
			f.hasDefer = true
		}
		return true
	})
	if f.hasDefer {
		st.deferFuncs++
	}
	st.funcs++
	l := fset.Position(body.End()).Line - fset.Position(body.Pos()).Line + 1
	st.lines += l
	st.sizes = append(st.sizes, l)
	f.block(body.List)
	return !f.panicSite
}

func main() {
	imp := importer.ForCompiler(fset, "source", nil)
	var pkgs []*pkgInfo
	for _, path := range os.Args[1:] {
		p, err := load(path, imp)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		pkgs = append(pkgs, p)
	}
	// Pass 1 finds the bodies that cannot unwind; leaf bodies first, so
	// iterate until nothing changes.
	for changed := true; changed; {
		changed = false
		for _, p := range pkgs {
			for _, file := range p.files {
				for _, d := range file.Decls {
					fd, ok := d.(*ast.FuncDecl)
					if !ok || fd.Body == nil {
						continue
					}
					name := fullName(p.info.Defs[fd.Name])
					if safe[name] {
						continue
					}
					var scratch stats
					if analyze(p, fd.Type, fd.Body, &scratch) {
						safe[name] = true
						changed = true
					}
				}
			}
		}
	}
	for _, p := range pkgs {
		for _, file := range p.files {
			for _, d := range file.Decls {
				if fd, ok := d.(*ast.FuncDecl); ok && fd.Body != nil {
					analyze(p, fd.Type, fd.Body, &total)
				}
			}
		}
	}
	t := total
	per := func(n int) string {
		return fmt.Sprintf("%6d  %.2f/func  %.1f/100 lines", n, float64(n)/float64(t.funcs), 100*float64(n)/float64(t.lines))
	}
	fmt.Printf("packages            %d\n", len(pkgs))
	fmt.Printf("functions           %d (incl. func literals), %d body lines\n", t.funcs, t.lines)
	fmt.Printf("calls               %s\n", per(t.calls))
	fmt.Printf("  may unwind        %s (%.0f%% of calls)\n", per(t.callsMayUnwind), 100*float64(t.callsMayUnwind)/float64(t.calls))
	fmt.Printf("  need a pad        %s (%.0f%% of calls, %.0f%% of unwinding calls)\n", per(t.callsPad),
		100*float64(t.callsPad)/float64(t.calls), 100*float64(t.callsPad)/float64(t.callsMayUnwind))
	fmt.Printf("builtin calls       %s\n", per(t.builtinCalls))
	fmt.Printf("bounds checks       %s\n", per(t.bounds))
	fmt.Printf("division checks     %s\n", per(t.divs))
	fmt.Printf("assertion checks    %s\n", per(t.asserts))
	fmt.Printf("nil checks (error)  %s\n", per(t.nilChecks))
	all := t.bounds + t.divs + t.asserts + t.nilChecks
	fmt.Printf("  panic sites w/pad %s (%.0f%% of panic sites)\n", per(t.panicsPad), 100*float64(t.panicsPad)/float64(all))
	fmt.Printf("small struct args   %s\n", per(t.structArgs))
	fmt.Printf("multi/struct result %s (of which (T, error): %d)\n", per(t.aggResults), t.errResult)
	fmt.Printf("funcs with defer    %s\n", per(t.deferFuncs))
	fmt.Printf("owning locals       %s\n", per(t.ownerLocals))
	fmt.Printf("if statements       %s\n", per(t.ifs))
	fmt.Printf("for statements      %s\n", per(t.loops))
	fmt.Printf("owners live at a padded call  %.2f on average\n", float64(t.liveAtPad)/float64(t.callsPad))
	sort.Ints(t.sizes)
	q := func(p float64) int { return t.sizes[int(p*float64(len(t.sizes)-1))] }
	fmt.Printf("body lines p10/p25/p50/p75/p90/p99  %d/%d/%d/%d/%d/%d\n", q(.10), q(.25), q(.5), q(.75), q(.9), q(.99))
}
