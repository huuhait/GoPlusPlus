// Command goast prints the tree that Go's parser builds for a Go file, in the
// form of `gopp parse -emit=ast`, so that the two parsers can be compared.
//
//	goast -dump FILE     the tree of one file
//	goast -hash          for each path on standard input, one line
//	                     "<hash of the tree> <path>", or "error <path>"
//	                     if Go's parser refuses the file
//
// The text of a literal is left out: its byte range says the same.
package main

import (
	"bufio"
	"bytes"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"hash/fnv"
	"os"
	"strings"
)

// Past this depth a line says how much deeper it is, as the dump of gopp does.
const maxIndent = 100

func main() {
	if len(os.Args) == 3 && os.Args[1] == "-dump" {
		text, err := dump(os.Args[2])
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		os.Stdout.Write(text)
		return
	}
	if len(os.Args) == 2 && os.Args[1] == "-hash" {
		out := bufio.NewWriter(os.Stdout)
		defer out.Flush()
		paths := bufio.NewScanner(os.Stdin)
		for paths.Scan() {
			text, err := dump(paths.Text())
			if err != nil {
				fmt.Fprintf(out, "error %s\n", paths.Text())
				continue
			}
			hash := fnv.New64a()
			hash.Write(text)
			fmt.Fprintf(out, "%016x %s\n", hash.Sum64(), paths.Text())
		}
		return
	}
	fmt.Fprintln(os.Stderr, "usage: goast -dump FILE | goast -hash < PATHS")
	os.Exit(2)
}

func dump(path string) ([]byte, error) {
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, path, nil, parser.SkipObjectResolution)
	if err != nil {
		return nil, err
	}
	d := &dumper{file: fset.File(file.Pos())}
	d.plain("File package=" + file.Name.Name)
	d.nested(func() {
		for _, decl := range file.Decls {
			d.decl(decl)
		}
	})
	return d.out.Bytes(), nil
}

type dumper struct {
	file   *token.File
	out    bytes.Buffer
	depth  int
	groups int
}

func (d *dumper) write(text string) {
	d.out.WriteString(strings.Repeat("  ", min(d.depth, maxIndent)))
	if d.depth > maxIndent {
		fmt.Fprintf(&d.out, "[+%d] ", d.depth-maxIndent)
	}
	d.out.WriteString(text)
}

func (d *dumper) plain(text string) {
	d.write(text)
	d.out.WriteByte('\n')
}

func (d *dumper) line(text string, lo, hi token.Pos) {
	d.write(text)
	fmt.Fprintf(&d.out, " @%d..%d\n", d.file.Offset(lo), d.file.Offset(hi))
}

func (d *dumper) node(text string, n ast.Node) {
	d.line(text, n.Pos(), n.End())
}

func (d *dumper) nested(body func()) {
	d.depth++
	body()
	d.depth--
}

func (d *dumper) field(name string, body func()) {
	d.plain(name + ":")
	d.nested(body)
}

func (d *dumper) exprs(name string, list []ast.Expr) {
	if len(list) > 0 {
		d.field(name, func() {
			for _, x := range list {
				d.expr(x)
			}
		})
	}
}

func (d *dumper) optExpr(name string, x ast.Expr) {
	if x != nil {
		d.field(name, func() { d.expr(x) })
	}
}

func (d *dumper) optStmt(name string, s ast.Stmt) {
	if s != nil {
		d.field(name, func() { d.stmt(s) })
	}
}

func (d *dumper) stmts(list []ast.Stmt) {
	for _, s := range list {
		d.stmt(s)
	}
}

func names(idents []*ast.Ident) string {
	list := make([]string, len(idents))
	for i, ident := range idents {
		list[i] = ident.Name
	}
	return strings.Join(list, ",")
}

// params prints one line per name of a parameter list, or one line for a
// parameter without a name, each with its type.
func (d *dumper) params(kind string, fields *ast.FieldList) {
	if fields == nil {
		return
	}
	for _, field := range fields.List {
		if len(field.Names) == 0 {
			d.plain(kind)
			d.nested(func() { d.expr(field.Type) })
		}
		for _, name := range field.Names {
			d.plain(kind + " " + name.Name)
			d.nested(func() { d.expr(field.Type) })
		}
	}
}

func (d *dumper) typeParams(fields *ast.FieldList) {
	if fields == nil {
		return
	}
	for _, field := range fields.List {
		for _, name := range field.Names {
			d.node("TypeParam "+name.Name, name)
			d.nested(func() { d.expr(field.Type) })
		}
	}
}

func (d *dumper) funcType(t *ast.FuncType, lo token.Pos) {
	d.line("FuncType", lo, t.End())
	d.nested(func() {
		d.params("Param", t.Params)
		d.params("Result", t.Results)
	})
}

func (d *dumper) block(b *ast.BlockStmt) {
	d.node("Block", b)
	d.nested(func() { d.stmts(b.List) })
}

func (d *dumper) decl(decl ast.Decl) {
	switch decl := decl.(type) {
	case *ast.FuncDecl:
		d.node("Func "+decl.Name.Name, decl)
		d.nested(func() {
			d.params("Receiver", decl.Recv)
			d.typeParams(decl.Type.TypeParams)
			d.funcType(decl.Type, decl.Pos())
			if decl.Body != nil {
				d.block(decl.Body)
			}
		})
	case *ast.GenDecl:
		group := ""
		if decl.Lparen.IsValid() {
			d.groups++
			group = fmt.Sprintf(" group=%d", d.groups)
		}
		for _, spec := range decl.Specs {
			// A declaration outside a group starts at its keyword.
			lo := spec.Pos()
			if !decl.Lparen.IsValid() {
				lo = decl.Pos()
			}
			d.spec(decl.Tok, spec, group, lo)
		}
	default:
		panic(fmt.Sprintf("declaration %T", decl))
	}
}

func (d *dumper) spec(keyword token.Token, spec ast.Spec, group string, lo token.Pos) {
	switch spec := spec.(type) {
	case *ast.ImportSpec:
		name := ""
		if spec.Name != nil {
			name = " " + spec.Name.Name
		}
		d.line("Import"+name+group, lo, spec.End())
		d.nested(func() { d.expr(spec.Path) })
	case *ast.ValueSpec:
		kind := "Var "
		if keyword == token.CONST {
			kind = "Const "
		}
		d.line(kind+names(spec.Names)+group, lo, spec.End())
		d.nested(func() {
			d.optExpr("type", spec.Type)
			d.exprs("values", spec.Values)
		})
	case *ast.TypeSpec:
		alias := ""
		if spec.Assign.IsValid() {
			alias = " alias"
		}
		d.line("Type "+spec.Name.Name+alias+group, lo, spec.End())
		d.nested(func() {
			d.typeParams(spec.TypeParams)
			d.expr(spec.Type)
		})
	default:
		panic(fmt.Sprintf("specification %T", spec))
	}
}

var unaryOps = map[token.Token]string{
	token.ADD:   "Plus",
	token.SUB:   "Neg",
	token.NOT:   "Not",
	token.XOR:   "Xor",
	token.AND:   "Addr",
	token.ARROW: "Recv",
	token.TILDE: "Tilde",
}

var binaryOps = map[token.Token]string{
	token.LOR:     "OrOr",
	token.LAND:    "AndAnd",
	token.EQL:     "Eq",
	token.NEQ:     "Ne",
	token.LSS:     "Lt",
	token.LEQ:     "Le",
	token.GTR:     "Gt",
	token.GEQ:     "Ge",
	token.ADD:     "Add",
	token.SUB:     "Sub",
	token.OR:      "Or",
	token.XOR:     "Xor",
	token.MUL:     "Mul",
	token.QUO:     "Div",
	token.REM:     "Rem",
	token.SHL:     "Shl",
	token.SHR:     "Shr",
	token.AND:     "And",
	token.AND_NOT: "AndNot",
}

var assignOps = map[token.Token]string{
	token.ASSIGN:         "Assign",
	token.DEFINE:         "Define",
	token.ADD_ASSIGN:     "Op(Add)",
	token.SUB_ASSIGN:     "Op(Sub)",
	token.MUL_ASSIGN:     "Op(Mul)",
	token.QUO_ASSIGN:     "Op(Div)",
	token.REM_ASSIGN:     "Op(Rem)",
	token.AND_ASSIGN:     "Op(And)",
	token.OR_ASSIGN:      "Op(Or)",
	token.XOR_ASSIGN:     "Op(Xor)",
	token.SHL_ASSIGN:     "Op(Shl)",
	token.SHR_ASSIGN:     "Op(Shr)",
	token.AND_NOT_ASSIGN: "Op(AndNot)",
}

var literalKinds = map[token.Token]string{
	token.INT:    "Int",
	token.FLOAT:  "Float",
	token.IMAG:   "Imag",
	token.CHAR:   "Rune",
	token.STRING: "String",
}

func (d *dumper) expr(x ast.Expr) {
	switch x := x.(type) {
	case *ast.Ident:
		d.node("Name "+x.Name, x)
	case *ast.BasicLit:
		d.node("BasicLit "+literalKinds[x.Kind], x)
	case *ast.CompositeLit:
		d.node("CompositeLit", x)
		d.nested(func() {
			d.optExpr("type", x.Type)
			for _, elem := range x.Elts {
				d.expr(elem)
			}
		})
	case *ast.KeyValueExpr:
		d.node("KeyValue", x)
		d.nested(func() {
			d.expr(x.Key)
			d.expr(x.Value)
		})
	case *ast.FuncLit:
		d.node("FuncLit", x)
		d.nested(func() {
			d.funcType(x.Type, x.Type.Pos())
			d.block(x.Body)
		})
	case *ast.ParenExpr:
		d.node("Paren", x)
		d.nested(func() { d.expr(x.X) })
	case *ast.SelectorExpr:
		d.node("Selector "+x.Sel.Name, x)
		d.nested(func() { d.expr(x.X) })
	case *ast.IndexExpr:
		d.node("Index", x)
		d.nested(func() {
			d.expr(x.X)
			d.exprs("indices", []ast.Expr{x.Index})
		})
	case *ast.IndexListExpr:
		d.node("Index", x)
		d.nested(func() {
			d.expr(x.X)
			d.exprs("indices", x.Indices)
		})
	case *ast.SliceExpr:
		full := ""
		if x.Slice3 {
			full = " full"
		}
		d.node("Slice"+full, x)
		d.nested(func() {
			d.expr(x.X)
			d.optExpr("lo", x.Low)
			d.optExpr("hi", x.High)
			d.optExpr("max", x.Max)
		})
	case *ast.TypeAssertExpr:
		// `x.(type)` outside the header of a switch.
		if x.Type == nil {
			d.node("TypeSwitchGuard", x)
			d.nested(func() { d.expr(x.X) })
			return
		}
		d.node("Assert", x)
		d.nested(func() {
			d.expr(x.X)
			d.expr(x.Type)
		})
	case *ast.CallExpr:
		spread := ""
		if x.Ellipsis.IsValid() {
			spread = " spread"
		}
		d.node("Call"+spread, x)
		d.nested(func() {
			d.expr(x.Fun)
			d.exprs("args", x.Args)
		})
	case *ast.StarExpr:
		d.node("Unary Star", x)
		d.nested(func() { d.expr(x.X) })
	case *ast.UnaryExpr:
		d.node("Unary "+unaryOps[x.Op], x)
		d.nested(func() { d.expr(x.X) })
	case *ast.BinaryExpr:
		d.node("Binary "+binaryOps[x.Op], x)
		d.nested(func() {
			d.expr(x.X)
			d.expr(x.Y)
		})
	case *ast.ArrayType:
		if x.Len == nil {
			d.node("SliceType", x)
			d.nested(func() { d.expr(x.Elt) })
			return
		}
		d.node("ArrayType", x)
		d.nested(func() {
			// `[...]T` has no length.
			if _, dots := x.Len.(*ast.Ellipsis); !dots {
				d.optExpr("len", x.Len)
			}
			d.expr(x.Elt)
		})
	case *ast.Ellipsis:
		d.node("DotsType", x)
		d.nested(func() { d.expr(x.Elt) })
	case *ast.StructType:
		d.node("StructType", x)
		d.nested(func() {
			for _, field := range x.Fields.List {
				d.node("Field "+names(field.Names), field)
				d.nested(func() {
					d.expr(field.Type)
					if field.Tag != nil {
						d.field("tag", func() { d.expr(field.Tag) })
					}
				})
			}
		})
	case *ast.InterfaceType:
		d.node("InterfaceType", x)
		d.nested(func() {
			for _, field := range x.Methods.List {
				if len(field.Names) == 0 {
					d.field("elem", func() { d.expr(field.Type) })
					continue
				}
				name := field.Names[0]
				d.node("Method "+name.Name, name)
				d.nested(func() { d.funcType(field.Type.(*ast.FuncType), name.Pos()) })
			}
		})
	case *ast.FuncType:
		d.funcType(x, x.Pos())
	case *ast.MapType:
		d.node("MapType", x)
		d.nested(func() {
			d.expr(x.Key)
			d.expr(x.Value)
		})
	case *ast.ChanType:
		dir := "Both"
		switch x.Dir {
		case ast.SEND:
			dir = "Send"
		case ast.RECV:
			dir = "Recv"
		}
		d.node("ChanType "+dir, x)
		d.nested(func() { d.expr(x.Value) })
	default:
		panic(fmt.Sprintf("expression %T", x))
	}
}

func (d *dumper) stmt(s ast.Stmt) {
	switch s := s.(type) {
	case *ast.EmptyStmt:
		d.node("EmptyStmt", s)
	case *ast.LabeledStmt:
		d.node("Labeled "+s.Label.Name, s)
		d.nested(func() { d.stmt(s.Stmt) })
	case *ast.BlockStmt:
		d.block(s)
	case *ast.ExprStmt:
		d.node("ExprStmt", s)
		d.nested(func() { d.expr(s.X) })
	case *ast.SendStmt:
		d.node("Send", s)
		d.nested(func() {
			d.expr(s.Chan)
			d.expr(s.Value)
		})
	case *ast.DeclStmt:
		d.node("DeclStmt", s)
		d.nested(func() { d.decl(s.Decl) })
	case *ast.AssignStmt:
		d.node("Assign "+assignOps[s.Tok], s)
		d.nested(func() {
			d.exprs("lhs", s.Lhs)
			d.exprs("rhs", s.Rhs)
		})
	case *ast.IncDecStmt:
		d.node("IncDec "+s.Tok.String(), s)
		d.nested(func() { d.expr(s.X) })
	case *ast.BranchStmt:
		kind := map[token.Token]string{
			token.BREAK:       "Break",
			token.CONTINUE:    "Continue",
			token.GOTO:        "Goto",
			token.FALLTHROUGH: "Fallthrough",
		}[s.Tok]
		if s.Label != nil {
			kind += " " + s.Label.Name
		}
		d.node(kind, s)
	case *ast.GoStmt:
		d.node("Go", s)
		d.nested(func() { d.expr(s.Call) })
	case *ast.DeferStmt:
		d.node("Defer", s)
		d.nested(func() { d.expr(s.Call) })
	case *ast.ReturnStmt:
		d.node("Return", s)
		d.nested(func() {
			for _, result := range s.Results {
				d.expr(result)
			}
		})
	case *ast.IfStmt:
		d.node("If", s)
		d.nested(func() {
			d.optStmt("init", s.Init)
			d.field("cond", func() { d.expr(s.Cond) })
			d.block(s.Body)
			d.optStmt("else", s.Else)
		})
	case *ast.ForStmt:
		d.node("For", s)
		d.nested(func() {
			d.optStmt("init", s.Init)
			d.optExpr("cond", s.Cond)
			d.optStmt("post", s.Post)
			d.block(s.Body)
		})
	case *ast.RangeStmt:
		define := ""
		if s.Tok == token.DEFINE {
			define = " define"
		}
		d.node("Range"+define, s)
		d.nested(func() {
			var lhs []ast.Expr
			for _, x := range []ast.Expr{s.Key, s.Value} {
				if x != nil {
					lhs = append(lhs, x)
				}
			}
			d.exprs("lhs", lhs)
			d.field("x", func() { d.expr(s.X) })
			d.block(s.Body)
		})
	case *ast.SwitchStmt:
		d.node("Switch", s)
		d.nested(func() {
			d.optStmt("init", s.Init)
			d.optExpr("tag", s.Tag)
			d.caseClauses(s.Body)
		})
	case *ast.TypeSwitchStmt:
		d.node("Switch", s)
		d.nested(func() {
			d.optStmt("init", s.Init)
			d.field("tag", func() { d.typeSwitchGuard(s.Assign) })
			d.caseClauses(s.Body)
		})
	case *ast.SelectStmt:
		d.node("Select", s)
		d.nested(func() {
			for _, clause := range s.Body.List {
				clause := clause.(*ast.CommClause)
				kind := "Comm"
				if clause.Comm == nil {
					kind = "Default"
				}
				d.node(kind, clause)
				d.nested(func() {
					d.optStmt("comm", clause.Comm)
					d.stmts(clause.Body)
				})
			}
		})
	default:
		panic(fmt.Sprintf("statement %T", s))
	}
}

func (d *dumper) caseClauses(body *ast.BlockStmt) {
	for _, clause := range body.List {
		clause := clause.(*ast.CaseClause)
		kind := "Case"
		if clause.List == nil {
			kind = "Default"
		}
		d.node(kind, clause)
		d.nested(func() {
			d.exprs("cases", clause.List)
			d.stmts(clause.Body)
		})
	}
}

// typeSwitchGuard prints `x := y.(type)` or `y.(type)`.
func (d *dumper) typeSwitchGuard(guard ast.Stmt) {
	name := ""
	var assert ast.Expr
	switch guard := guard.(type) {
	case *ast.AssignStmt:
		name = " " + guard.Lhs[0].(*ast.Ident).Name
		assert = guard.Rhs[0]
	case *ast.ExprStmt:
		assert = guard.X
	}
	d.node("TypeSwitchGuard"+name, guard)
	d.nested(func() { d.expr(assert.(*ast.TypeAssertExpr).X) })
}
