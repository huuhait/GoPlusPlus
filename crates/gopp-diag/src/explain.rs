//! What each diagnostic means, how to fix it, and an example: the text of
//! `gopp explain`.

use std::fmt::Write;

use crate::{Code, LockRegistry, Severity};

/// The text for one code.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Explanation {
    pub meaning: &'static str,
    pub fix: &'static str,
    /// Source code, one statement per line.
    pub example: &'static str,
}

/// What a row of the registry is to this compiler.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// An error or a warning that is reported.
    Reported(Code),
    /// A note, printed under the diagnostic it belongs to.
    Note(Code),
    /// A row that this version reports nothing with.
    Reserved,
    /// A row of a feature that the language no longer has.
    Retired,
}

/// The rows of features that the language no longer has. Nothing is
/// reported with them, and their numbers are not used again.
const RETIRED: [&str; 45] = [
    "err_view_for_ref",
    "err_sema_assign",
    "err_sema_builtin",
    "err_sema_call",
    "err_sema_decl",
    "err_sema_expr",
    "err_sema_index",
    "err_sema_predicate",
    "err_sema_stmt",
    "err_sema_typeset",
    "err_sema_typexpr",
    "err_strict_placement",
    "err_option_not_strict",
    "warn_vet_strict_no_required",
    "err_view_field_without_attr",
    "err_view_write",
    "err_view_no_methods",
    "err_view_to_owned",
    "err_view_bad_arg",
    "err_view_copy_pointer",
    "err_unit_in_func",
    "err_unit_cycle",
    "err_unit_factor",
    "err_unit_dimension",
    "err_unit_not_unit",
    "err_unit_is_not_expr",
    "err_unit_expr",
    "err_unit_type",
    "err_unit_mismatch",
    "err_unit_plain",
    "err_unit_inexact",
    "err_unit_conv",
    "err_unit_op",
    "err_unit_symbol",
    "err_unit_scale",
    "err_unit_op_method",
    "err_unit_in",
    "err_unit_generic",
    // A copy is written `=`: no value has a `Clone` to call (ADR-0045).
    "note_own_fix_clone",
    "err_own_clone_not_cloneable",
    "err_own_clone_ambiguous",
    "err_own_clone_ptr_receiver",
    // What an assertion or `Get` gives is a copy, not a binding that is
    // only read (ADR-0041).
    "err_own_binding_read_only",
    "note_own_fix_binding_copy",
    "err_own_get_binding_read_only",
];

/// What the row `number` of the registry is; `None` if there is no such
/// row.
pub fn status(lock: &LockRegistry, number: u16) -> Option<Status> {
    let name = lock.name(number)?;
    let code = Code::ALL.iter().find(|code| code.number() == number);
    Some(match code {
        Some(&code) if code.severity() == Severity::Note => Status::Note(code),
        Some(&code) => Status::Reported(code),
        None if RETIRED.contains(&name) => Status::Retired,
        None => Status::Reserved,
    })
}

/// The row that `query` names: `E0111`, `111` or `err_undefined`.
fn row(lock: &LockRegistry, query: &str) -> Option<u16> {
    let digits = query.strip_prefix(['E', 'e']).unwrap_or(query);
    match digits.parse::<u16>() {
        Ok(number) => lock.name(number).map(|_| number),
        Err(_) => lock.number(query),
    }
}

/// The text of `gopp explain <query>`; `Err` says that the query names
/// no code.
pub fn explained(query: &str) -> Result<String, String> {
    let lock = LockRegistry::built_in();
    let number = row(&lock, query)
        .ok_or_else(|| format!("{query} is no error code; gopp explain --list lists them"))?;
    let name = lock.name(number).unwrap_or_default();
    let mut text = format!("E{number:04} {name}\n\n");
    match status(&lock, number) {
        Some(Status::Reported(code)) => match explain(code) {
            Some(explanation) => {
                let _ = writeln!(
                    text,
                    "{}\n\nFix: {}\n\nExample:\n",
                    explanation.meaning, explanation.fix
                );
                for line in explanation.example.lines() {
                    let _ = writeln!(text, "    {line}");
                }
            }
            None => text.push_str("No explanation is written for this code yet.\n"),
        },
        Some(Status::Note(_)) => {
            text.push_str("A note: it is printed under the error that it belongs to.\n");
        }
        Some(Status::Retired) => {
            text.push_str("Retired: the language no longer has what this code was for.\n");
        }
        Some(Status::Reserved) | None => {
            text.push_str("Reserved: this version of gopp reports nothing with this code.\n");
        }
    }
    Ok(text)
}

/// The text of `gopp explain --list`: every row, and what it is if it is
/// no error or warning that is reported.
pub fn listed() -> String {
    let lock = LockRegistry::built_in();
    let mut text = String::new();
    for number in 1..=lock.len() as u16 {
        let name = lock.name(number).unwrap_or_default();
        let what = match status(&lock, number) {
            Some(Status::Retired) => " (retired)",
            Some(Status::Reserved) => " (reserved)",
            _ => "",
        };
        let _ = writeln!(text, "E{number:04} {name}{what}");
    }
    text
}

/// What the code means, how to fix it, and an example; `None` for a note
/// and for a code without a text yet.
pub fn explain(code: Code) -> Option<Explanation> {
    use Code::*;
    let (meaning, fix, example) = match code {
        TooManyErrors => (
            "The compiler stops reporting after a number of errors, because later ones are often \
             caused by earlier ones.",
            "Fix the first errors and compile again.",
            "x := undefined1 + undefined2 // ... and many more",
        ),
        RangeAliasAssign => (
            "`&v` in a range clause declares a new name for the element itself, so it is only \
             written with `:=`.",
            "Use `:=`, or drop the `&` to assign a copy of each element.",
            "for _, &d = range docs {}  // error\nfor _, &d := range docs {} // ok",
        ),
        RangeAliasKey => (
            "Only the element of a range clause can be named with `&`. A key or an index is a \
             value of its own, not a place in the collection.",
            "Remove the `&` from the first variable.",
            "for &i, d := range docs {}  // error\nfor i, &d := range docs {}  // ok",
        ),
        AttrOnStmt => (
            "An attribute is written before a top-level declaration or after the type of a \
             struct field. A statement has none.",
            "Move the attribute to the declaration it is meant for.",
            "func f() {\n\t@Inline x := 1 // error\n}",
        ),
        AttrOnImport => (
            "An import can only have `@Build`, which says when the import is part of the build.",
            "Remove the attribute, or use `@Build(cond)`.",
            "@Inline\nimport \"os\"       // error\n\n@Build(linux)\nimport \"syscall\"  // ok",
        ),
        AddrOfCompositeLit => (
            "In a .gopp file `&T{...}` does not put a value on the heap: a pointer `*T` borrows \
             a value that something else owns.",
            "Use `NewUnique(T{...})` for one owner or `NewShared(T{...})` for several.",
            "p := &Node{}           // error\np := NewUnique(Node{}) // ok",
        ),
        FailsReturnsError => (
            "A function declared with `!` already returns an error after its results. Writing \
             `error` as the last result too would return two.",
            "Remove the `error` result.",
            "func load!(path string) (Data, error) // error\nfunc load!(path string) Data          // ok",
        ),
        LoopVarRedeclared => (
            "A loop variable is declared again inside its own loop. The inner one would hide \
             the loop variable, which is easy to misread.",
            "Give the inner variable another name.",
            "for _, u := range users {\n\tu := u    // error\n\tmine := u // ok\n}",
        ),
        Undefined => (
            "A name is used that is not declared in this scope, in the package, or in an \
             imported package.",
            "Declare it, import its package, or fix the spelling.",
            "func f() int { return count } // error: undefined: count",
        ),
        Redeclared => (
            "A name is declared twice in one scope.",
            "Rename one of them, or use `=` to assign to the first.",
            "x := 1\nx := 2 // error\nx = 2  // ok",
        ),
        DeclaredNotUsed => (
            "A local variable is declared and never read.",
            "Remove it, or assign the value to `_` if only the side effect is wanted.",
            "func f() {\n\tn := compute() // error: declared and not used: n\n\t_ = compute()  // ok\n}",
        ),
        UnexportedName => (
            "A name that starts with a lower-case letter belongs to its package and cannot be \
             used from another one.",
            "Use an exported name of that package.",
            "x := strings.builder{} // error\nx := strings.Builder{} // ok",
        ),
        InvalidRecursiveType => (
            "A type holds a value of itself, so it would have no size.",
            "Hold a pointer, a `Unique[T]`, a slice or an `Option` of a pointer instead.",
            "type Node struct{ next Node }                  // error\ntype Node struct{ next Option[Unique[Node]] }  // ok",
        ),
        InitCycle => (
            "Package variables are initialized in the order of what they use. Here the \
             initial values use each other in a circle.",
            "Break the circle: set one of the variables in an `init` function.",
            "var a = b + 1\nvar b = a + 1 // error: initialization cycle",
        ),
        CannotConvert => (
            "`T(x)` is written for a value that has no conversion to `T`.",
            "Convert through a type that both sides share, or call a function that does the \
             conversion.",
            "n := int(\"12\")           // error\nn := strconv.Atoi!(\"12\") // ok",
        ),
        MismatchedTypes => (
            "The two operands of an operator have different types. Go++ does not convert \
             between them by itself.",
            "Convert one side to the type of the other.",
            "var a int32 = 1\nvar b int64 = 2\n_ = a + b        // error\n_ = int64(a) + b // ok",
        ),
        CannotCompare => (
            "The values cannot be compared with `==` or `!=`: their type holds a slice, a map \
             or a func, or one side is `nil` and the other has no `nil`.",
            "Compare the parts that can be compared, or write a function that does.",
            "a, b := []int{1}, []int{1}\n_ = a == b               // error\n_ = slices.Equal(a, b)   // ok",
        ),
        DivisionByZero => (
            "A constant expression divides by zero.",
            "Fix the divisor.",
            "const n = 10 / 0 // error",
        ),
        UndefinedFieldOrMethod => (
            "The selector names a field or a method that the type does not have. In a .gopp \
             file the members of an embedded field are not reached through the outer value.",
            "Fix the name, or select the field that has the member first.",
            "var p Point\n_ = p.Z // error: p.Z undefined (type Point has no field or method Z)",
        ),
        NotEnoughArguments => (
            "A call has fewer arguments than the function has parameters.",
            "Pass the missing arguments.",
            "func add(a, b int) int\n_ = add(1)    // error\n_ = add(1, 2) // ok",
        ),
        TooManyArguments => (
            "A call has more arguments than the function has parameters.",
            "Remove the extra arguments.",
            "func add(a, b int) int\n_ = add(1, 2, 3) // error",
        ),
        IndexOutOfBounds => (
            "A constant index or slice bound is outside a length that is written in the \
             program.",
            "Fix the index.",
            "var a [3]int\n_ = a[3] // error: the last element is a[2]",
        ),
        AddressOfUnaddressable => (
            "`&x` needs a variable, a field, or an element. A temporary such as a call result \
             has no address.",
            "Store the value in a variable first.",
            "p := &f()   // error\nv := f()\np := &v     // ok",
        ),
        CannotAssign => (
            "A value is used where a value of another type is needed: in an assignment, as an \
             argument, as a result, or in a literal.",
            "Convert the value, or change the type of the place.",
            "var n int = \"three\" // error\nvar n int = 3       // ok",
        ),
        CannotAssignTo => (
            "The left side of an assignment is not a place that can be written: it is a \
             constant, a call result, or a field of a map entry.",
            "Assign to a variable; for a map entry, write `for k, &v := range m { v.f = x }` or \
             store the whole value.",
            "m[k].count = 1 // error\nv := m[k]\nv.count = 1\nm[k] = v       // ok",
        ),
        AssignMismatch => (
            "An assignment has a different number of places on the left than values on the \
             right.",
            "Match the counts; use `_` for a value that is not needed.",
            "a, b := pair()    // ok: pair returns two values\na := pair()       // error\na, _ := pair()    // ok",
        ),
        DefineNoNewVars => (
            "`:=` declares at least one new variable, and every name on the left exists \
             already.",
            "Use `=`.",
            "x := 1\nx := 2 // error\nx = 2  // ok",
        ),
        MissingReturn => (
            "A function with results can reach its end without a `return`.",
            "Return a value on every path.",
            "func sign(n int) int {\n\tif n < 0 {\n\t\treturn -1\n\t}\n} // error: missing return",
        ),
        DuplicateCase => (
            "Two cases of one `switch` have the same constant value, so the second can never \
             run.",
            "Remove or change one of them.",
            "switch n {\ncase 1:\ncase 1: // error\n}",
        ),
        OwnR3aNeedsOwnership => (
            "A value of a type that cannot be copied is read out of a place that keeps it: a \
             field, an element, a map entry, a package variable, a captured variable, or what a \
             pointer points to. A copy cannot be made, and the place is never emptied by a \
             plain read.",
            "Borrow it with `&place`, take it out with `mem.Take(&place)` (which leaves the \
             zero value) or with `mem.Replace(&place, v)`.",
            "f := w.Out            // error: File cannot be copied\nf := mem.Take(&w.Out) // ok: w.Out is now the zero File\ncheck(&w.Out)         // ok: borrowed",
        ),
        OwnBorrowedAddress => (
            "The address of a plain range variable is taken. The variable is a copy of the \
             element (or, for a type that cannot be copied, a read-only view of it), so a \
             pointer to it would not change the collection.",
            "Write `&v` in the range clause: `v` is then the element itself.",
            "for _, d := range docs { fix(&d) }  // error\nfor _, &d := range docs { fix(&d) } // ok",
        ),
        OwnMoveNotPlace => (
            "`move(x)` takes a local variable or a parameter. Its operand here is a temporary, \
             which is handed over without `move`.",
            "Remove the `move`.",
            "keep(move(open())) // error\nkeep(open())       // ok",
        ),
        OwnDropSignature => (
            "The method named `drop` is the destructor of its type. It has a pointer receiver, \
             no parameters and no results.",
            "Declare it as `func (t *T) drop()`, or give the method another name.",
            "func (f File) drop()        // error\nfunc (f *File) drop() error // error\nfunc (f *File) drop() {}    // ok",
        ),
        OwnDropCalled => (
            "`drop` runs by itself when a value ends. It cannot be called or used as a value.",
            "Let the value end: leave its block, assign over it, or `move` it away.",
            "f.drop() // error",
        ),
        OwnRangeVarWrite => (
            "A part of a plain range variable is changed. The variable is a copy of the \
             element, so the change would be lost when the iteration ends. For a type that \
             cannot be copied the variable is a read-only view of the element.",
            "Write `&v` in the range clause to change the element itself.",
            "for _, d := range docs { d.Title = \"x\" }  // error\nfor _, &d := range docs { d.Title = \"x\" } // ok",
        ),
        OwnRangeAliasInvalid => (
            "`&v` in a range clause names an element of a slice, an array or a map. A string, \
             an integer and a channel have no element to name.",
            "Remove the `&`.",
            "for _, &c := range \"text\" {} // error\nfor _, c := range \"text\" {}  // ok",
        ),
        OwnRangeAliasFunc => (
            "`&v` over an iterator function names what the yielded pointer points to, so the \
             iterator must yield pointers.",
            "Remove the `&`, or make the iterator yield `*T`.",
            "for &v := range values {} // error if values yields T, not *T",
        ),
        OwnPinnedCopy => (
            "A value of a `@Pinned` type stays where it was created: it is not copied.",
            "Pass a pointer to it.",
            "var mu Mutex\nother := mu // error\nuse(&mu)    // ok",
        ),
        OwnPinnedMove => (
            "A value of a `@Pinned` type stays where it was created: it is not moved, also not \
             with `mem.Take` or `mem.Replace`.",
            "Pass a pointer to it, or build the value where it is needed.",
            "var mu Mutex\nkeep(move(mu)) // error\nkeep(&mu)      // ok, if keep takes *Mutex",
        ),
        OwnGenericCopy => (
            "A value whose type is a type parameter is copied, and the constraint does not say \
             that the type can be copied. The type argument may be a type such as a file.",
            "Hand the value over with `move(v)`, borrow it with `&v`, or constrain the type \
             parameter with `Clone` (or `Copy`).",
            "func twice[T any](v T) (T, T)   { return v, v } // error\nfunc twice[T Clone](v T) (T, T) { return v, v } // ok",
        ),
        OwnWeakArg => (
            "`NewWeak` makes a weak handle from a `Shared[T]`.",
            "Pass a `Shared[T]`.",
            "u := NewUnique(Node{})\nw := NewWeak(u) // error\ns := NewShared(Node{})\nw := NewWeak(s) // ok",
        ),
        OwnMoveRangeBorrowed => (
            "A plain range variable of a type that cannot be copied is a read-only view of \
             the element. It cannot be moved away: the element stays in the collection.",
            "Write `&v` in the range clause and take the element with `mem.Take(&v)` or \
             `mem.Replace(&v, other)`.",
            "for _, f := range files { keep(move(f)) }       // error\nfor _, &f := range files { keep(mem.Take(&f)) } // ok",
        ),
        BangNoErrorResult => (
            "`f!(x)` unwraps the error that `f` returns as its last result. This call returns \
             no error.",
            "Remove the `!`.",
            "n := len!(xs) // error\nn := len(xs)  // ok",
        ),
        BangOnConversion => (
            "`T!(x)` is written for a conversion. A conversion returns no error to unwrap.",
            "Remove the `!`.",
            "n := int!(x) // error\nn := int(x)  // ok",
        ),
        UnusedImport => (
            "A package is imported and never used.",
            "Remove the import, or import it as `_` if only its initialization is wanted.",
            "import \"os\" // error: \"os\" imported and not used",
        ),
        NoZeroValue => (
            "The operation leaves or creates a zero value, and the type has none: a pointer, a \
             `Unique`, a `Shared`, a func, an interface with methods, or a struct with a \
             `@Required` field.",
            "With `mem.Take`, use `mem.Replace(&place, v)` and give the value to leave behind; \
             otherwise use `Option[T]`.",
            "old := mem.Take(&c.Owner)                       // error: Unique[Widget] has no zero value\nold := mem.Replace(&c.Owner, NewUnique(Widget{})) // ok",
        ),
        RequiredPlacement => (
            "`@Required` is written on a field of a struct, after its type.",
            "Move it to the field.",
            "@Required\nvar id int // error\n\ntype User struct {\n\tID int @Required // ok\n}",
        ),
        MoveNonlocal => (
            "`move(x)` takes a local variable or a parameter. A field, an element, a package \
             variable, a variable captured by a closure and an `&` range variable stay in \
             place, because something else still names them.",
            "Take the value with `mem.Take(&place)` or `mem.Replace(&place, v)`.",
            "keep(move(w.Out))      // error\nkeep(mem.Take(&w.Out)) // ok",
        ),
        FailReturnNil => (
            "A Go-style function returns `nil` for a result whose type is never nil (a \
             pointer, a func, an interface with methods).",
            "Declare the function with `!`, which needs no placeholder on failure, or return \
             an `Option`.",
            "func find(id int) (*Node, error) { return nil, errMissing } // error\nfunc find!(id int) *Node         { return errMissing }      // ok",
        ),
        RequiredArgs => (
            "`@Required` takes no arguments.",
            "Remove the parentheses.",
            "ID int @Required(true) // error\nID int @Required       // ok",
        ),
        NilNeverNil => (
            "`nil` is used with a type that is never nil in a .gopp file: a pointer, a \
             `Unique`, a `Shared`, a func, or an interface with methods.",
            "Use `Option[T]` for a value that may be missing.",
            "var next *Node = nil         // error\nvar next Option[*Node] = nil // ok",
        ),
        AttrNoArgs => (
            "The attribute takes no arguments.",
            "Remove the parentheses.",
            "@Inline(true)\nfunc f() {} // error\n\n@Inline\nfunc g() {} // ok",
        ),
        ExtendsOneParent => (
            "`@Extends` names one parent. A type with several parents has one `@Extends` for \
             each, in the order of the parents.",
            "Write one `@Extends(Parent)` per parent.",
            "@Extends(A, B)\ntype C struct{} // error\n\n@Extends(A)\n@Extends(B)\ntype D struct{} // ok",
        ),
        AbstractAttrPlacement => (
            "`@Abstract` is written on a method with a pointer receiver, or on a type.",
            "Move it, or give the method a pointer receiver.",
            "@Abstract\nfunc (s Shape) Area() float64  // error\n\n@Abstract\nfunc (s *Shape) Area() float64 // ok",
        ),
        ConvertMisuse => (
            "`@Convert` marks a method that converts its receiver: a value receiver, no \
             parameters and one result.",
            "Give the method that shape.",
            "@Convert\nfunc (m Meters) Float() float64 { return float64(m.n) } // ok: float64(m) calls it",
        ),
        RightMisuse => (
            "`@Right` lets a binary `@Operator` method serve `s op a`, where `s` has the type \
             of the parameter. It is written on a method that is marked `@Operator` and has \
             two operands.",
            "Add `@Operator`, or remove `@Right`.",
            "@Operator\n@Right\nfunc (v Vec) Mul(k float64) Vec // ok: 2.0 * v",
        ),
        GirExternBad => (
            "`@Extern(\"C\")` declares a function that is defined in C: it takes the string \
             \"C\", and the function has no body and no type parameters.",
            "Declare the function without a body.",
            "@Extern(\"C\")\nfunc puts(s *byte) int32 // ok",
        ),
        GirExportBad => (
            "`@Export(\"C\")` gives a function a C symbol so that C can call it: it takes the \
             string \"C\", and the function has a body and no type parameters.",
            "Give the function a body, and write it without type parameters.",
            "@Export(\"C\")\nfunc add(a, b int32) int32 { return a + b } // ok",
        ),
        InlineMarksConflict => (
            "A function is marked `@Inline` and `@NoInline`.",
            "Keep one of them.",
            "@Inline\n@NoInline\nfunc f() {} // error",
        ),
        OwnStarOnLeft => (
            "In a .gopp file a pointer is written through without a star: assigning a `T` to a \
             `*T` variable changes what it points to.",
            "Remove the star.",
            "func set(p *int) {\n\t*p = 9 // error\n\tp = 9  // ok\n}",
        ),
        OwnCopyToCopy => (
            "`copy(&dst, src)` writes into `dst`. Passing `dst` without `&` would fill a copy \
             that is thrown away.",
            "Pass `&dst`.",
            "copy(dst, src)  // error\ncopy(&dst, src) // ok",
        ),
        TypeArgUnsatisfied => (
            "A type argument does not satisfy the constraint of its type parameter: it lacks a \
             method, is not in the type set, or cannot be copied where the constraint asks for \
             `Copy` or `Clone`.",
            "Use a type that satisfies the constraint, or loosen the constraint.",
            "func max[T cmp.Ordered](a, b T) T\n_ = max(f, g) // error if f and g are funcs",
        ),
        CannotInfer => (
            "The type arguments of a generic function cannot be worked out from its \
             arguments.",
            "Write them: `f[int](...)`.",
            "func zero[T any]() T\nx := zero()      // error: cannot infer T\nx := zero[int]() // ok",
        ),
        BangNoZeroResult => (
            "`f!(x)` in a Go-style function returns early with the error, and the other \
             results get their zero values. Here a result has a type with no zero value.",
            "Declare the function with `!`, or handle the error and return a value.",
            "func load(id int) (*Node, error) {\n\tn := find!(id) // error: no *Node to return\n\treturn n, nil\n}\nfunc load!(id int) *Node { return find!(id) } // ok",
        ),
        OwnCopyLocal => (
            "A variable of a type that cannot be copied is used where its value gets a new \
             owner: an assignment, an argument, a field of a literal, a send. No copy can be \
             made.",
            "Hand the value over with `move(x)`, or lend it with `&x`.",
            "f := open()\ng := f       // error: File cannot be copied\ng := move(f) // ok: f cannot be read after this\ncheck(&f)    // ok: borrowed",
        ),
        AttrPlacement => (
            "A built-in attribute is written on something it does not apply to.",
            "Move it to what it is for; `docs/attributes.md` lists where each one goes.",
            "@Packed\nfunc f() {}      // error\n\n@Packed\ntype T struct{}  // ok",
        ),
        AttrArgs => (
            "The arguments of an attribute are wrong: a built-in attribute takes other \
             arguments, or an argument is not known at compile time.",
            "Pass what the attribute takes: constants, literals, names and types, no variables.",
            "@Align(3)\ntype T int // error: a power of two\n\n@Align(8)\ntype U int // ok",
        ),
        LexIllegalNul => (
            "The file has a NUL byte, which no source file may have.",
            "Remove the byte; the file may be a binary file or damaged.",
            "// a file that holds the byte 0x00",
        ),
        LexUtf16 => (
            "The file is stored as UTF-16. Source files are UTF-8.",
            "Save the file as UTF-8.",
            "// a file that starts with the bytes FF FE",
        ),
        LexIllegalUtf8 => (
            "The file has bytes that are not valid UTF-8.",
            "Save the file as UTF-8, or write the character as an escape in a string.",
            "s := \"\\xff\" // ok: the byte is written as an escape",
        ),
        LexIllegalBom => (
            "A byte order mark stands somewhere other than at the start of the file.",
            "Remove it.",
            "// U+FEFF in the middle of a file",
        ),
        LexIllegalChar => (
            "A character stands where no token can start with it.",
            "Remove it, or put it inside a string or a comment.",
            "x := 1 $ 2 // error: illegal character '$'",
        ),
        LexCurlyQuote => (
            "A curly quotation mark is used where a string or a rune is meant. They often come from a word processor.",
            "Use the plain quotes \" and '.",
            "s := “text” // error\ns := \"text\" // ok",
        ),
        LexCommentUnterminated => (
            "A comment that starts with /* has no */ before the end of the file.",
            "Close the comment.",
            "/* not closed",
        ),
        LexRadixPoint => (
            "A binary or octal number has a decimal point. Only decimal and hexadecimal numbers can have one.",
            "Write the number in decimal or hexadecimal.",
            "x := 0b1.1 // error\nx := 1.5   // ok",
        ),
        LexNoDigits => (
            "A number has a base prefix and no digits after it.",
            "Write the digits.",
            "x := 0x  // error\nx := 0x1 // ok",
        ),
        LexExponentNeedsDecimal => (
            "The exponent `e` belongs to a decimal number. This number has another base.",
            "Use `p` for a hexadecimal number, or write the number in decimal.",
            "x := 0b1e2 // error\nx := 1e2   // ok",
        ),
        LexExponentNeedsHex => (
            "The exponent `p` belongs to a hexadecimal number.",
            "Use `e` for a decimal number.",
            "x := 1p3    // error\nx := 0x1p3  // ok",
        ),
        LexExponentNoDigits => (
            "An exponent has no digits.",
            "Write the digits of the exponent.",
            "x := 1e  // error\nx := 1e3 // ok",
        ),
        LexHexMantissaNeedsP => (
            "A hexadecimal number with a point needs an exponent written with `p`.",
            "Add the exponent.",
            "x := 0x1.8   // error\nx := 0x1.8p0 // ok",
        ),
        LexInvalidDigit => (
            "A digit does not belong to the base of its number.",
            "Use digits of the base.",
            "x := 0b102 // error\nx := 0o78  // error",
        ),
        LexBadSeparator => (
            "An underscore in a number stands somewhere other than between two digits.",
            "Put `_` only between digits, or after the base prefix.",
            "x := 1__000 // error\nx := 1_000  // ok",
        ),
        LexEscapeUnknown => (
            "A backslash is followed by a character that makes no escape.",
            "Use a known escape (\\n, \\t, \\\\, \\\", \\xNN, \\uNNNN), or write two backslashes.",
            "s := \"C:\\dir\"  // error\ns := \"C:\\\\dir\" // ok",
        ),
        LexEscapeIllegalChar => (
            "A numeric escape has a character that is no digit of its base, or too few digits.",
            "Write all the digits: two after \\x, four after \\u, eight after \\U, three octal ones after \\.",
            "s := \"\\x4\"  // error\ns := \"\\x41\" // ok",
        ),
        LexEscapeBadCodePoint => (
            "An escape names a number that is no Unicode character: a surrogate half, or a value above U+10FFFF.",
            "Use a valid code point.",
            "r := '\\uD800' // error",
        ),
        LexRuneUnterminated => (
            "A rune literal is not closed before the end of the line.",
            "Add the closing quote.",
            "r := 'a // error",
        ),
        LexRuneIllegal => (
            "A rune literal holds no character, or more than one.",
            "Write one character, or use a string.",
            "r := ''   // error\nr := 'ab' // error\ns := \"ab\" // ok",
        ),
        LexStringUnterminated => (
            "A string is not closed before the end of the line.",
            "Add the closing quote; a string over several lines is written with back quotes.",
            "s := \"text // error",
        ),
        LexRawStringUnterminated => (
            "A string that starts with a back quote has no closing back quote.",
            "Add the closing back quote.",
            "s := `text",
        ),
        Expected => (
            "The parser needs a certain token here and found the end of the input or something else.",
            "Write what is named in the message.",
            "func f( { } // error: expected )",
        ),
        ExpectedFound => (
            "The parser needs a certain token here and found another one.",
            "Write what is named in the message, or remove what was found.",
            "x := [1, 2 // error: expected ], found newline",
        ),
        MissingComma => (
            "Two items of a list have no comma between them.",
            "Add the comma.",
            "f(a b)  // error\nf(a, b) // ok",
        ),
        MissingCommaNewline => (
            "The last item of a list that ends on the next line needs a comma after it.",
            "Add a comma after the last item, or put the closing bracket on the same line.",
            "xs := []int{\n\t1,\n\t2   // error: missing comma before newline\n}",
        ),
        NestingTooDeep => (
            "Blocks or types are nested deeper than the parser takes.",
            "Split the code into more functions or more type declarations.",
            "// thousands of { inside each other",
        ),
        ExpressionTooDeeplyNested => (
            "An expression is nested deeper than the parser takes.",
            "Split the expression with variables.",
            "// thousands of ( inside each other",
        ),
        UnexpectedCommaRbrack => (
            "A comma stands where the list it would continue cannot go on.",
            "Remove the comma.",
            "var a [3,]int // error",
        ),
        ParenEmbeddedType => (
            "An embedded type is written in parentheses.",
            "Remove the parentheses.",
            "type T struct{ (io.Reader) } // error",
        ),
        ParenComplitType => (
            "The type of a composite literal is written in parentheses.",
            "Remove the parentheses.",
            "p := (Point){1, 2} // error\np := Point{1, 2}   // ok",
        ),
        MissingTypeConstraint => (
            "A type parameter has no constraint.",
            "Write a constraint; `any` allows every type.",
            "func f[T]() {}     // error\nfunc f[T any]() {} // ok",
        ),
        MissingTypeParamName => (
            "A type parameter list has a constraint without a name.",
            "Write the name before the constraint.",
            "func f[any]() {}   // error\nfunc f[T any]() {} // ok",
        ),
        MissingTypeParamNameOrLen => (
            "After the name of a type, `[` starts a list of type parameters or the length of an array, and neither is written.",
            "Write `[T any]` for a generic type or `[N]T` for an array.",
            "type T[] int // error",
        ),
        MissingParamType => (
            "A parameter has a name and no type.",
            "Write the type.",
            "func f(a, b) {}     // error\nfunc f(a, b int) {} // ok",
        ),
        MissingParamName => (
            "A parameter list has both parameters with names and parameters without.",
            "Give every parameter a name, or none.",
            "func f(a int, string) {}   // error\nfunc f(a int, s string) {} // ok",
        ),
        DotsInvalid => (
            "`...` stands where it has no meaning: without a type, or on a parameter that is not the last.",
            "Use `...T` for the last parameter only.",
            "func f(xs ...int, n int) {} // error\nfunc f(n int, xs ...int) {} // ok",
        ),
        EmptyTypeParams => (
            "A type parameter list is empty.",
            "Remove the brackets, or declare a type parameter.",
            "func f[]() {} // error",
        ),
        FuncTypeTypeParams => (
            "A function type or a function literal has type parameters. Only a declared function can be generic.",
            "Declare a generic function with a name.",
            "f := func[T any](x T) {} // error",
        ),
        MethodTypeParams => (
            "A method is declared with type parameters in a Go file. Go has no generic methods.",
            "Make it a generic function, or move it to a .gopp file.",
            "func (s Set) Map[T any](f func(int) T) []T // error in a .go file",
        ),
        InterfaceMethodTypeParams => (
            "A method of an interface has type parameters.",
            "Remove them; an interface can be generic as a whole: `type I[T any] interface{ M(T) }`.",
            "type I interface{ M[T any](T) } // error",
        ),
        Slice3MiddleIndex => (
            "A slice expression with two colons needs its middle index.",
            "Write `x[lo:hi:max]` with `hi`.",
            "s := x[1::3]  // error\ns := x[1:2:3] // ok",
        ),
        Slice3FinalIndex => (
            "A slice expression with two colons needs its last index.",
            "Write `x[lo:hi:max]` with `max`.",
            "s := x[1:2:]  // error\ns := x[1:2:3] // ok",
        ),
        CallParenthesized => (
            "The call of a `go` or `defer` statement is written in parentheses.",
            "Remove the parentheses.",
            "defer (f()) // error\ndefer f()   // ok",
        ),
        MissingIfCondition => (
            "An `if` statement has no condition.",
            "Write the condition.",
            "if x := f(); { } // error\nif x := f(); x > 0 { }",
        ),
        VarInIfInit => (
            "A `var` declaration stands in the header of an `if`, `for` or `switch`.",
            "Use `:=`.",
            "if var x = f(); x > 0 { } // error\nif x := f(); x > 0 { }    // ok",
        ),
        NewlineAfterIfClause => (
            "A line ends inside the header of an `if`, `for` or `switch`, where a semicolon is inserted.",
            "Keep the header and its `{` on one line.",
            "if x > 0\n{ // error\n}",
        ),
        NewlineBeforeLbrace => (
            "The `{` of a block stands on a new line.",
            "Put `{` at the end of the line before.",
            "func f()\n{ // error\n}",
        ),
        ImportPathNotString => (
            "An import path is not a string.",
            "Write the path in quotes.",
            "import os   // error\nimport \"os\" // ok",
        ),
        MissingImportPath => (
            "An import has a name and no path.",
            "Write the path.",
            "import m // error\nimport m \"math\"",
        ),
        ImportAfterDecl => (
            "An import stands after another declaration. Imports come first in a file.",
            "Move the import to the top.",
            "var x int\nimport \"os\" // error",
        ),
        MacroNameLowercase => (
            "The name of a macro starts with a capital letter, so that `#Name` and `@Name` are easy to see.",
            "Rename the macro.",
            "macro assert(cond expr) { } // error\nmacro Assert(cond expr) { } // ok",
        ),
        UnknownMacroKind => (
            "A macro parameter has a kind that does not exist.",
            "Use one of the kinds the message lists (expr, ident, lit, stmt, block, decl, type, tokens).",
            "macro M(x thing) { } // error",
        ),
        GoppDirectiveInSource => (
            "A `//gopp:` comment in a source file names a setting that belongs to the module file.",
            "Write the line the note gives in gopp.mod.",
            "//gopp:profile debug   // error in a source file",
        ),
        StmtOutsideFunc => (
            "A statement stands outside any function. At the top of a file only declarations are allowed.",
            "Move the statement into a function, such as `init` or `main`.",
            "package p\nx := 1      // error\nvar x = 1   // ok",
        ),
        ExpectedLbraceAfter => (
            "The block of a statement is missing: the parser found something else where `{` belongs.",
            "Write the block.",
            "if x > 0 return // error\nif x > 0 { return }",
        ),
        BadBuildConstraint => (
            "A build constraint (`//go:build` or `@Build`) is not a valid condition.",
            "Write words joined by `&&`, `||` and `!`, with parentheses.",
            "//go:build linux &&   // error\n//go:build linux && amd64",
        ),
        BadImportPath => (
            "An import path has characters that are not allowed, or is empty.",
            "Fix the path.",
            "import \"a b\"  // error\nimport \"\"     // error",
        ),
        LabelNotUsed => (
            "A label is declared and no `goto`, `break` or `continue` names it.",
            "Remove the label.",
            "Loop: // error: label Loop defined and not used\n\tfor { }",
        ),
        LabelRedeclared => (
            "Two labels of one function have the same name.",
            "Rename one.",
            "L: x++\nL: y++ // error",
        ),
        LabelUndeclared => (
            "A `goto`, `break` or `continue` names a label that the function does not have.",
            "Declare the label or fix its name.",
            "goto Done // error: label Done not defined",
        ),
        InvalidBreakLabel => (
            "`break L` names a label that is not on a loop, `switch` or `select` around the statement.",
            "Put the label on the statement to leave.",
            "L: x++\nfor { break L } // error",
        ),
        InvalidContinueLabel => (
            "`continue L` names a label that is not on a loop around the statement.",
            "Put the label on the loop.",
            "L: switch { }\nfor { continue L } // error",
        ),
        GotoJumpsOver => (
            "A `goto` jumps forward over the declaration of a variable that is still in scope at the label.",
            "Move the declaration before the `goto`, or put it in a block of its own.",
            "func f() {\n\tgoto End\n\tx := 1 // error: goto End jumps over this declaration\nEnd:\n\tuse(x)\n}",
        ),
        GotoJumpsInto => (
            "A `goto` jumps into a block from outside it.",
            "Move the label out of the block.",
            "func f() {\n\tgoto In // error\n\t{\n\tIn:\n\t}\n}",
        ),
        NotInLoopBreak => (
            "`break` stands outside any loop, `switch` or `select`.",
            "Remove it, or use `return`.",
            "func f() { break } // error",
        ),
        NotInLoopContinue => (
            "`continue` stands outside any loop.",
            "Remove it.",
            "func f() { continue } // error",
        ),
        FallthroughMisplaced => (
            "`fallthrough` is the last statement of a case of an expression `switch`. Here it stands elsewhere.",
            "Move it to the end of the case.",
            "switch x {\ncase 1:\n\tif y { fallthrough } // error\n}",
        ),
        FallthroughFinal => (
            "`fallthrough` in the last case has no next case to go to.",
            "Remove it.",
            "switch x {\ncase 1:\n\tfallthrough // error\n}",
        ),
        FallthroughTypeSwitch => (
            "`fallthrough` is not allowed in a type switch: the variable has another type in each case.",
            "List the types in one case: `case A, B:`.",
            "switch v.(type) {\ncase int:\n\tfallthrough // error\ncase string:\n}",
        ),
        CannotUseBlank => (
            "`_` is used as a value or as a type. It can only be assigned to.",
            "Use a name.",
            "x := _ // error\n_ = x  // ok",
        ),
        InitSignature => (
            "`init` has no parameters and no results.",
            "Remove them.",
            "func init(n int) {} // error\nfunc init() {}      // ok",
        ),
        InitWithBody => (
            "`init` needs a body, and is declared only as a function.",
            "Write `func init() { ... }`.",
            "func init() // error: missing function body\nvar init = 1 // error",
        ),
        InitTypeParams => (
            "`init` and `main` cannot have type parameters.",
            "Remove them.",
            "func main[T any]() {} // error",
        ),
        DuplicateMethod => (
            "A type has two methods with the same name.",
            "Rename one, or remove it.",
            "func (t T) M() {}\nfunc (t T) M() {} // error: method T.M already declared",
        ),
        FieldAndMethod => (
            "A type has a field and a method with the same name.",
            "Rename one of them.",
            "type T struct{ Name string }\nfunc (t T) Name() string { return \"\" } // error",
        ),
        ImportUnresolved => (
            "The imported package cannot be found.",
            "Fix the path, or add the module that has the package.",
            "import \"no/such/pkg\" // error",
        ),
        NotAType => (
            "A name is used as a type and denotes something else: a variable, a constant, a function or a package.",
            "Use the name of a type.",
            "var n int\nvar x n // error: n is not a type",
        ),
        NotAnExpression => (
            "A type is used where a value is needed.",
            "Use a value of the type: a literal, a conversion, a variable.",
            "x := int   // error: int (type) is not an expression\nx := int(0)",
        ),
        InvalidRecursiveTypeSelf => (
            "A type is declared as itself.",
            "Declare it as another type.",
            "type T T // error: invalid recursive type: T refers to itself",
        ),
        InvalidCycle => (
            "A declaration needs itself: a constant whose value uses the constant, or a type whose size uses the type.",
            "Break the circle.",
            "const a = b\nconst b = a // error",
        ),
        InitCycleSelf => (
            "The initial value of a package variable uses the variable itself.",
            "Set the variable in an `init` function, or do not use it in its own value.",
            "var x = f()\nfunc f() int { return x } // error: initialization cycle: x refers to itself",
        ),
        InvalidArrayLength => (
            "The length of an array is not a constant integer that is zero or more.",
            "Use such a constant.",
            "var a [-1]int   // error\nvar b [1.5]int  // error",
        ),
        ArrayLengthNotConstant => (
            "The length of an array is not a constant.",
            "Use a constant, or a slice: `make([]T, n)`.",
            "n := 3\nvar a [n]int         // error\na := make([]int, n)  // ok",
        ),
        InvalidMapKey => (
            "The key type of a map cannot be compared with `==`: it is or holds a slice, a map or a func.",
            "Use a key type that can be compared, such as a string or a struct of comparable fields.",
            "var m map[[]int]string // error\nvar m map[string]string",
        ),
        EmbeddedFieldPointerIface => (
            "An embedded field is a pointer to an interface type.",
            "Embed the interface itself.",
            "type T struct{ *io.Reader } // error\ntype T struct{ io.Reader }  // ok",
        ),
        EmbeddedFieldPointer => (
            "An embedded field is a pointer type by itself, not a named type or a pointer to one.",
            "Give the field a name, or embed the type that is pointed to.",
            "type P *int\ntype T struct{ P } // error",
        ),
        EmbeddedFieldUnsafePointer => (
            "`unsafe.Pointer` cannot be an embedded field.",
            "Give the field a name.",
            "type T struct{ unsafe.Pointer }   // error\ntype T struct{ p unsafe.Pointer } // ok",
        ),
        EmbeddedFieldTypeParam => (
            "A type parameter, or a pointer to one, cannot be an embedded field.",
            "Give the field a name.",
            "type T[P any] struct{ P }   // error\ntype T[P any] struct{ v P } // ok",
        ),
        InvalidReceiver => (
            "The receiver of a method is not a type that can have methods: it must be a type declared in this package, or a pointer to one, and not itself a pointer or an interface.",
            "Declare the method on a type of this package.",
            "func (n int) Double() int   // error: int is not declared here\ntype N int\nfunc (n N) Double() N       // ok",
        ),
        ReceiverMethodPkg => (
            "A method is declared on a type of another package.",
            "Declare a type of your own (`type Mine other.T`) and put the method on it, or write a function.",
            "func (b strings.Builder) Len2() int // error",
        ),
        ReceiverCount => (
            "A method has more than one receiver.",
            "Write one receiver.",
            "func (a, b T) M() {} // error",
        ),
        ReceiverMissing => (
            "The parentheses of a receiver are empty.",
            "Write the receiver, or remove the parentheses to declare a function.",
            "func () M() {} // error",
        ),
        UseOfUntypedNil => (
            "`nil` is used where nothing gives it a type.",
            "Declare the variable with a type.",
            "x := nil          // error\nvar x Option[int] // ok: x is nil",
        ),
        ValueTruncated => (
            "A constant with a fraction is used where an integer is needed.",
            "Use an integer constant, or convert a variable at run time.",
            "var n int = 1.5 // error: 1.5 (untyped float constant) truncated to int\nvar n int = 1",
        ),
        ValueOverflows => (
            "A constant does not fit the type it is given.",
            "Use a larger type or a smaller constant.",
            "var b uint8 = 256 // error: 256 overflows uint8",
        ),
        GenericWithoutInstantiation => (
            "A generic function or type is used without its type arguments where they cannot be worked out.",
            "Write the type arguments.",
            "var f = Map       // error\nvar f = Map[int]  // ok",
        ),
        OperatorNotDefined => (
            "The operator is not defined for the type of its operand.",
            "Use an operand of a type that has the operator, or convert it.",
            "s := \"a\" - \"b\" // error: operator - not defined on string\nb := !1        // error",
        ),
        ShiftCountNegative => (
            "The count of a shift is a negative constant.",
            "Use a count that is zero or more.",
            "x := 1 << -1 // error",
        ),
        ShiftOperandInvalid => (
            "The value that is shifted is not an integer.",
            "Convert it to an integer type first.",
            "x := 1.5 << 2 // error",
        ),
        ShiftCountInvalid => (
            "The count of a shift is not an integer, or is too large to be a count.",
            "Use an integer count of a sensible size.",
            "x := 1 << 1.5 // error",
        ),
        ConstantExpressionOverflow => (
            "A constant expression gets too large for the compiler to keep exact.",
            "Make the constant smaller.",
            "const big = 1 << 100000 // error",
        ),
        NotConstant => (
            "A value that is not constant is used where a constant is needed: in a `const` declaration, an array length or a case that must be constant.",
            "Use a constant, or declare a variable.",
            "n := 3\nconst c = n // error\nvar c = n   // ok",
        ),
        NoValue => (
            "A call of a function without a result is used as a value.",
            "Do not use the call as a value, or make the function return one.",
            "func log(s string) {}\nx := log(\"a\") // error: log(\"a\") (no value) used as value",
        ),
        MultiValueInSingle => (
            "A call that returns several values is used where one value is needed.",
            "Assign the results to variables first.",
            "func pair() (int, int)\nx := pair() + 1 // error\na, b := pair()  // ok",
        ),
        BuiltinNotCalled => (
            "A built-in function is used as a value. It can only be called.",
            "Call it, or wrap it in a function of your own.",
            "f := len                               // error: len (built-in) must be called\nf := func(s string) int { return len(s) } // ok",
        ),
        NotEnoughReturnValues => (
            "A `return` has fewer values than the function has results.",
            "Return a value for every result.",
            "func f() (int, string) { return 1 } // error",
        ),
        TooManyReturnValues => (
            "A `return` has more values than the function has results.",
            "Remove the extra values, or add results to the function.",
            "func f() int { return 1, \"a\" } // error",
        ),
        CannotIndex => (
            "The value cannot be indexed: it is not an array, a slice, a string or a map, nor a pointer to one.",
            "Index something that has elements.",
            "n := 5\nx := n[0] // error",
        ),
        CannotSlice => (
            "The value cannot be sliced: it is not a string, an array or a slice.",
            "Slice something that has elements.",
            "m := map[string]int{}\nx := m[1:2] // error",
        ),
        CannotSliceUnaddressable => (
            "An array that is a temporary is sliced. Slicing an array needs a variable.",
            "Store the array in a variable first.",
            "s := f()[:] // error if f returns an array\na := f()\ns := a[:]   // ok",
        ),
        SliceOfString => (
            "A string is sliced with three indices. A string has no capacity.",
            "Use two indices.",
            "t := s[1:2:3] // error\nt := s[1:2]   // ok",
        ),
        SliceIndicesInverted => (
            "Constant slice bounds are in the wrong order: a lower bound is larger than an upper one.",
            "Swap the bounds.",
            "t := s[3:1] // error",
        ),
        LiteralTooFew => (
            "A struct literal without field names lists fewer values than the struct has fields.",
            "List every field, or use names: `T{X: 1}`.",
            "type Point struct{ X, Y int }\np := Point{1}    // error\np := Point{X: 1} // ok",
        ),
        LiteralTooMany => (
            "A literal lists more values than its type has fields or elements.",
            "Remove the extra values.",
            "type Point struct{ X, Y int }\np := Point{1, 2, 3} // error",
        ),
        LiteralMixture => (
            "A struct literal mixes values with field names and values without.",
            "Use names for all the fields, or for none.",
            "p := Point{X: 1, 2}    // error\np := Point{X: 1, Y: 2} // ok",
        ),
        LiteralDuplicateField => (
            "A struct literal sets one field twice.",
            "Remove one of them.",
            "p := Point{X: 1, X: 2} // error",
        ),
        LiteralMissingKey => (
            "A map literal has an element without a key.",
            "Write `key: value` for every element.",
            "m := map[string]int{\"a\": 1, 2} // error",
        ),
        LiteralDuplicateKey => (
            "A map literal has the same constant key twice.",
            "Remove one of them.",
            "m := map[string]int{\"a\": 1, \"a\": 2} // error",
        ),
        LiteralDuplicateIndex => (
            "An array or slice literal sets one index twice.",
            "Remove one of them.",
            "a := []int{0: 1, 0: 2} // error",
        ),
        LiteralIndexNotConstant => (
            "An index in an array or slice literal is not a constant integer that is zero or more.",
            "Use such a constant.",
            "i := 1\na := []int{i: 5} // error",
        ),
        LiteralUnexportedField => (
            "A struct literal without field names is written for a struct of another package that has fields which are not exported.",
            "Use field names for the exported fields, or a constructor of that package.",
            "t := other.T{1, 2} // error: implicit assignment to unexported field",
        ),
        LiteralInvalidFieldName => (
            "A key in a struct literal is not the name of a field.",
            "Write the field name.",
            "p := Point{\"X\": 1} // error\np := Point{X: 1}   // ok",
        ),
        LiteralArrayTooLong => (
            "An array literal has more elements than the array has, or an index past its end.",
            "Remove elements, or make the array longer.",
            "a := [2]int{1, 2, 3} // error",
        ),
        LiteralMissingType => (
            "A composite literal has no type, and nothing around it gives one.",
            "Write the type before the braces.",
            "x := {1, 2}      // error\nx := []int{1, 2} // ok",
        ),
        NotAnInterface => (
            "A type assertion or a type switch is written on a value that is not of an interface type.",
            "Assert on an interface value; a value of a known type needs no assertion.",
            "n := 5\ns := n.(string) // error: n is not an interface",
        ),
        TypeAssertionImpossible => (
            "The asserted type can never be in the interface value: it does not have the methods of the interface.",
            "Assert a type that implements the interface.",
            "var r io.Reader\nn := r.(int) // error: int does not implement io.Reader",
        ),
        TypeSwitchOutside => (
            "`x.(type)` is used outside the header of a `switch`.",
            "Write `switch v := x.(type) { ... }`.",
            "t := x.(type) // error",
        ),
        DerefNonPointer => (
            "`*x` is written for a value that is not a pointer.",
            "Remove the star.",
            "n := 5\nm := *n // error: cannot indirect n",
        ),
        MissingInitExpr => (
            "A declaration has more names than values.",
            "Give every name a value, or none.",
            "var a, b = 1    // error\nvar a, b = 1, 2 // ok",
        ),
        ExtraInitExpr => (
            "A declaration has more values than names.",
            "Remove the extra values, or add names.",
            "var a = 1, 2 // error",
        ),
        DuplicateTypeCase => (
            "A type switch lists one type twice.",
            "Remove one of them.",
            "switch v.(type) {\ncase int:\ncase int: // error\n}",
        ),
        SwitchTagNotComparable => (
            "The value of a `switch` cannot be compared with `==`, so no case can be tested against it.",
            "Switch on something comparable, or use a `switch` without a value and write the conditions.",
            "switch xs { // error if xs is a slice\ncase nil:\n}",
        ),
        SelectCaseInvalid => (
            "A case of a `select` is not a send or a receive.",
            "Write `case v := <-ch:` or `case ch <- v:`.",
            "select {\ncase x > 0: // error\n}",
        ),
        RangeCannot => (
            "The value after `range` cannot be ranged over.",
            "Range over an array, a slice, a string, a map, a channel, an integer or an iterator function.",
            "var p Point\nfor x := range p { } // error",
        ),
        GoDeferNeedsCall => (
            "`go` and `defer` take a function call.",
            "Call the function; for several statements, call a function literal.",
            "defer f              // error\ndefer f()            // ok\ndefer func() { a(); b() }()",
        ),
        IncDecNonNumeric => (
            "`++` and `--` need a numeric variable.",
            "Use a number.",
            "s := \"a\"\ns++ // error",
        ),
        BuiltinNotEnoughArguments => (
            "A built-in function is called with too few arguments.",
            "Pass the arguments it needs.",
            "n := len()       // error\ns := append()    // error",
        ),
        BuiltinTooManyArguments => (
            "A built-in function is called with too many arguments.",
            "Remove the extra arguments.",
            "n := len(a, b) // error",
        ),
        BuiltinInvalidArgument => (
            "An argument of a built-in function has a type that the function does not take.",
            "Pass a value the function works on.",
            "n := len(5)   // error: invalid argument: 5 for built-in len\nclose(xs)     // error if xs is not a channel",
        ),
        CannotUseEllipsis => (
            "`...` after the last argument spreads a slice over a variadic parameter. Here the function is not variadic, or the argument is a call with several results.",
            "Remove the `...`, or pass a slice to a variadic function.",
            "func f(a, b int)\nf(xs...) // error",
        ),
        InvalidUseOfDots => (
            "`[...]T` lets a composite literal count its elements. It is used somewhere else.",
            "Write the length, or use it with a literal.",
            "var a [...]int        // error\na := [...]int{1, 2}   // ok",
        ),
        NotAGenericType => (
            "Type arguments are written for a type that has no type parameters.",
            "Remove the type arguments.",
            "type T int\nvar x T[string] // error",
        ),
        ShadowedResult => (
            "A bare `return` would return the named results, but one of their names denotes another variable at that point.",
            "Return the values by name, or rename the inner variable.",
            "func f() (n int) {\n\t{\n\t\tn := 1\n\t\treturn // error: result parameter n not in scope at return\n\t}\n}",
        ),
        IotaOutsideConst => (
            "`iota` is used outside a constant declaration.",
            "Use it in a `const` block.",
            "var x = iota // error\nconst (\n\tA = iota // ok\n)",
        ),
        InvalidConstantType => (
            "A constant is declared with a type that constants cannot have. Constants are numbers, strings and booleans.",
            "Use `var`, or a basic type.",
            "const p *int = nil     // error\nconst xs = []int{1, 2} // error",
        ),
        MethodExprPointerReceiver => (
            "A method with a pointer receiver is used on a value that has no address, or as `T.m` instead of `(*T).m`.",
            "Call it on a variable, or write `(*T).m`.",
            "f := T.Set    // error: needs pointer receiver\nf := (*T).Set // ok",
        ),
        AmbiguousSelector => (
            "The name is found in two embedded fields at the same depth, so the selector could mean either.",
            "Name the embedded field first.",
            "type C struct{ A; B } // A and B both have Name\nc.Name   // error\nc.A.Name // ok",
        ),
        GotNTypeArgs => (
            "A generic type is given the wrong number of type arguments.",
            "Give one for each type parameter.",
            "type Pair[K, V any] struct{}\nvar p Pair[int] // error",
        ),
        GotNTypeArgsFunc => (
            "A generic function is given more type arguments than it has type parameters.",
            "Remove the extra type arguments.",
            "func f[T any](x T) {}\nf[int, string](1) // error",
        ),
        CannotUseConstraintOutside => (
            "An interface with a type list (`~int | string`) or with `comparable` is a constraint. It cannot be the type of a variable.",
            "Use it as the constraint of a type parameter.",
            "type Number interface{ ~int | ~float64 }\nvar n Number             // error\nfunc sum[T Number](xs []T) T // ok",
        ),
        InterfaceContainsTypeConstraints => (
            "An interface that lists types is used as the type of a value.",
            "Use it only as a constraint.",
            "var x interface{ int | string } // error",
        ),
        InterfaceIsComparable => (
            "`comparable`, or an interface that embeds it, is used as the type of a value.",
            "Use it only as a constraint.",
            "var x comparable // error",
        ),
        UnionWithInterface => (
            "A union of types (`A | B`) has a term that is an interface with methods.",
            "List only types without methods in a union.",
            "type C interface{ int | fmt.Stringer } // error",
        ),
        UnionComparable => (
            "A union of types has `comparable`, or an interface that embeds it, as a term.",
            "Embed `comparable` on a line of its own instead.",
            "type C interface{ int | comparable } // error\ntype C interface{ comparable; int | string }",
        ),
        UnionTypeParam => (
            "A term of a union, or an embedded element, is a type parameter.",
            "List types, not type parameters.",
            "func f[T any, C interface{ T | int }]() {} // error",
        ),
        UnionOverlapping => (
            "Two terms of a union cover the same types.",
            "Remove one of them.",
            "type C interface{ ~int | int } // error",
        ),
        UnionTildeUnderlying => (
            "`~T` needs a `T` that is its own underlying type.",
            "Write `~` before the underlying type.",
            "type My int\ntype C interface{ ~My }  // error\ntype C interface{ ~int } // ok",
        ),
        UnionTildeInterface => (
            "`~` is written before an interface type.",
            "Remove the `~`.",
            "type C interface{ ~error } // error",
        ),
        TypeParamBoundNotInterface => (
            "A constraint that is a type parameter cannot be used as it is.",
            "Wrap it in an interface, or use another constraint.",
            "func f[T any, U T]() {} // error",
        ),
        InvalidReceiverTypeParams => (
            "The receiver of a method of a generic type names the type parameters wrongly: the count differs, or they are not plain names.",
            "Write one new name for each type parameter of the type.",
            "type List[T any] struct{}\nfunc (l List[int]) Len() int // error\nfunc (l List[T]) Len() int   // ok",
        ),
        DuplicateMethodIface => (
            "An interface has two methods with the same name.",
            "Remove one of them.",
            "type I interface {\n\tM()\n\tM() // error\n}",
        ),
        UnusedImportAs => (
            "A package is imported under a name, and the name is never used.",
            "Remove the import.",
            "import m \"math\" // error: \"math\" imported as m and not used",
        ),
        StructEmbedding => (
            "A struct type is malformed where a field name or an embedded type is expected.",
            "Write `name Type` or an embedded type name.",
            "type T struct{ 1 } // error",
        ),
        UnusedExpression => (
            "An expression stands as a statement and its value is thrown away.",
            "Assign the value, or remove the expression.",
            "x + 1     // error: x + 1 is not used\nlen(xs)   // error\n_ = x + 1 // ok",
        ),
        InvalidIndex => (
            "An index is not an integer, or is a negative constant.",
            "Use an integer that is zero or more.",
            "x := xs[1.5] // error\nx := xs[-1]  // error",
        ),
        LiteralUnknownField => (
            "A struct literal names a field that the struct does not have.",
            "Fix the field name.",
            "p := Point{Z: 1} // error: unknown field Z in struct literal of type Point",
        ),
        LiteralInvalidType => (
            "A composite literal is written for a type that has no literal: not a struct, an array, a slice or a map.",
            "Use a conversion or a plain value.",
            "n := int{1} // error\nn := int(1) // ok",
        ),
        ChannelDirection => (
            "A channel is used against its direction: a send on a receive-only channel, or a receive from a send-only one.",
            "Use a channel with the right direction.",
            "func f(in <-chan int) { in <- 1 }   // error\nfunc g(out chan<- int) { x := <-out } // error",
        ),
        AssertedTypeParam => (
            "A type assertion or a type switch is written on a value whose type is a type parameter.",
            "Convert the value to `any` first.",
            "func f[T any](v T) {\n\t_ = v.(int)      // error\n\t_ = any(v).(int) // ok\n}",
        ),
        MalformedConstant => (
            "A constant literal cannot be read as a number.",
            "Fix the literal.",
            "x := 0x1p    // error\nx := 1e+     // error",
        ),
        TildeOutsideConstraint => (
            "`~T` is written outside a constraint interface.",
            "Use `~T` only inside `interface{ ... }` that is a constraint.",
            "var x ~int // error",
        ),
        TypeTooLarge => (
            "A type is larger than a program can hold.",
            "Make it smaller.",
            "var a [1 << 62]int64 // error",
        ),
        NonBooleanCondition => (
            "The condition of an `if` or a `for` is not a boolean.",
            "Compare the value to get a boolean.",
            "n := 3\nif n { }      // error\nif n != 0 { } // ok",
        ),
        MultipleDefaults => (
            "A `switch` or a `select` has more than one `default`.",
            "Keep one.",
            "switch x {\ndefault:\ndefault: // error\n}",
        ),
        RangeVariables => (
            "A range clause has more variables than the ranged value gives.",
            "Remove the extra variables.",
            "for i, v, w := range xs { } // error\nfor i := range ch { }       // ok: a channel gives one value",
        ),
        UnionTooManyTerms => (
            "A union of types has more terms than the compiler takes.",
            "Split the constraint into several interfaces.",
            "// interface{ T1 | T2 | ... } with more than 100 terms",
        ),
        CannotCall => (
            "The value that is called is not a function.",
            "Call a function, or remove the parentheses.",
            "n := 5\nn() // error: cannot call non-function n",
        ),
        NoZeroLookup => (
            "An entry of a map may be missing and a channel may be closed. Go gives the zero \
             value then, and this type has none.",
            "Read with the second value and test it: `v, ok := m[k]`, `v, ok := <-ch`.",
            "u := users[\"bob\"]     // error: User has no zero value\nu, ok := users[\"bob\"] // ok: u is used where ok is true",
        ),
        RequiredMissing => (
            "A struct literal leaves out a field that must be set: the field is `@Required`, or \
             its type has no zero value.",
            "Set every field that the message names.",
            "type User struct {\n\tID   int @Required\n\tNote string\n}\nu := User{Note: \"x\"} // error: missing ID\nu := User{ID: 1}     // ok",
        ),
        RequiredPointer => (
            "`@Required` is written on a field whose type is never nil already: a pointer, a \
             `Unique`, a `Shared`, a `Weak`, a func, or an interface with methods. Such a field \
             must be set in every literal anyway.",
            "Remove `@Required`.",
            "type Node struct {\n\tNext *Node @Required // error\n\tPrev *Node           // ok: must be set all the same\n}",
        ),
        OptionAndRequired => (
            "`@Required` is written on an `Option` field. `Option[T]` says that the value may \
             be missing, and `@Required` says that it may not.",
            "Write `T @Required`, or `Option[T]` without the attribute.",
            "type User struct {\n\tNick Option[string] @Required // error\n\tName string @Required         // ok\n}",
        ),
        RequiredNilStore => (
            "`nil` is stored in a `@Required` field of a map, channel, `error` or `any` type. \
             Such a field is never nil.",
            "Store a value, or declare the field as an `Option`.",
            "type Job struct {\n\tDone chan bool @Required\n}\nj.Done = nil                // error\nj.Done = make(chan bool, 1) // ok",
        ),
        RequiredMove => (
            "`mem.Take` would leave nil in a `@Required` field of a map, channel, `error` or \
             `any` type, which is never nil.",
            "Use `mem.Replace(&place, v)` with a value that is set.",
            "old := mem.Take(&j.Done)                        // error\nold := mem.Replace(&j.Done, make(chan bool, 1)) // ok",
        ),
        RequiredAddr => (
            "The address of a `@Required` field of a map, channel, `error` or `any` type is \
             taken. Through the pointer nil could be stored in a field that is never nil.",
            "Read the field, or replace its value with `mem.Replace(&place, v)`.",
            "p := &j.Done                                    // error\nold := mem.Replace(&j.Done, make(chan bool, 1)) // ok",
        ),
        RequiredMaybeNil => (
            "A value that may be nil is stored in a `@Required` field of a map, channel, \
             `error` or `any` type, which is never nil.",
            "Store a value that is known to be set: a literal, `make(...)`, a value of a type \
             that is never nil, another such field, or a local variable.",
            "j.Done = pick()             // error: the result may be nil\nch := pick()\nj.Done = ch                 // ok where ch is known to be set",
        ),
        OwnRefIntoTemp => (
            "A `Unique[T]` or a `Shared[T]` that is a temporary is used as a `*T`. The \
             temporary ends with its statement, and the pointer would point at nothing.",
            "Store the value in a variable first.",
            "var p *Node = open() // error: open returns a Unique[Node]\nu := open()\nvar p *Node = u      // ok",
        ),
        OwnSharedWrite => (
            "What a `Shared[T]` holds is changed through the handle. Several handles are the \
             same value, so it is only read through them, and so is a pointer taken into it.",
            "Put the part that changes in a `Mutex` or an `Atomic`, or own the value with a \
             `Unique[T]`.",
            "func rename(s Shared[Node]) {\n\ts.Name = \"x\" // error\n\t_ = s.Name   // ok\n}",
        ),
        InstOption => (
            "`Option[T]` is written for a type that has `nil` already: a slice, a map, a \
             channel, `error`, `any`, `unsafe.Pointer`, or another `Option`. It is also \
             reported where such a type is the type argument of a generic type or function \
             that writes `Option[T]` for its type parameter.",
            "Use the type itself: its `nil` already says that nothing is there.",
            "var a Option[[]int] // error\nvar a []int         // ok: nil when empty",
        ),
        OptionMaybeNil => (
            "A variable was tested before a loop, and the loop assigns it. On a later turn of \
             the loop the test no longer says anything.",
            "Test the variable inside the loop, or do not assign it there.",
            "if o == nil {\n\treturn\n}\nfor i := 0; i < n; i++ {\n\tuse(o.Name) // error\n\to = next()\n}",
        ),
        ValueUnchecked => (
            "The value of a lookup or of a call is used where it may not be set. Its type has \
             no zero value, so it is a value only where `ok` is true or `err` is nil.",
            "Leave when the lookup or the call failed, and use the value after that.",
            "u, ok := users[name]\nshow(u)   // error: u is set only where ok is true\nif !ok {\n\treturn\n}\nshow(u)   // ok",
        ),
        MapTempWrite => (
            "An entry is written in a map that no variable holds: the result of a call. The \
             map ends with the statement, and the entry with it.",
            "Store the map in a variable first.",
            "load()[\"a\"] = 1 // error\nm := load()\nm[\"a\"] = 1      // ok",
        ),
        BoxWrite => (
            "An element is written in the map or slice that a type assertion gives. The \
             assertion gives a copy of what the interface holds, so the write would be lost.",
            "Put the asserted value in a variable, change that, and store it back if the \
             interface should hold the change.",
            "v.(map[string]int)[\"a\"] = 1 // error\nm := v.(map[string]int)\nm[\"a\"] = 1\nv = m                        // ok",
        ),
        CloseNilChan => (
            "`close` is called on a channel variable that is nil on every way to the call. \
             Closing a nil channel panics.",
            "Make the channel before it is closed.",
            "var ch chan int\nclose(ch)            // error\nch = make(chan int)\nclose(ch)            // ok",
        ),
        InvalidHandleType => (
            "A predeclared generic type is written for a type it does not take. `Local[T]` \
             ties a func or an interface value to one goroutine, so `T` is a func or an \
             interface type. `Unique`, `Shared` and `Weak` keep a value on the heap, and a \
             pointer only borrows: it is not kept there.",
            "Use `Local` with a func or an interface type; keep the value itself on the heap, \
             not a pointer to it.",
            "var a Local[int]     // error\nvar b Local[func()]  // ok\nvar c Unique[*Node]  // error\nvar d Unique[Node]   // ok",
        ),
        OwnBoxNeedsOwnership => (
            "An interface value holds a copy of what is put into it, so a value of a type \
             that cannot be copied is not converted to an interface type, also not with \
             `move`. For the same reason no interface value holds one, and it is not asserted.",
            "Put the value in a `Shared[T]`, or pass `&x` to an interface parameter for the \
             length of the call.",
            "f := open()\nvar a any = f                  // error: a File cannot be copied\nvar b any = NewShared(move(f)) // ok\nfmt.Println(&g)                // ok: g is lent for the call",
        ),
        IfaceNotSend => (
            "A value is kept in an interface variable, field, element or result, and its \
             type may not be given to another goroutine. An ordinary interface value may be \
             sent anywhere, so what it holds is `Send`; an interface that embeds `Sync`, such \
             as `error`, holds what is `Sync` too. A raw pointer is neither.",
            "Keep it in a `Local[I]`, which stays with one goroutine, or hold a `Shared[T]`; an \
             error type is converted by value.",
            "var r Reader = &buf        // error: a *Buffer borrows what it points to\nvar r Local[Reader] = &buf // ok\nreturn 0, &errBad          // error\nreturn 0, errBad           // ok",
        ),
        _ => return None,
    };
    Some(Explanation {
        meaning,
        fix,
        example,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_error_and_warning_has_a_text() {
        let reported = |code: &&Code| code.severity() != Severity::Note;
        let without = Code::ALL
            .iter()
            .filter(reported)
            .filter(|&&code| explain(code).is_none());
        let without: Vec<&str> = without.map(|code| code.name()).collect();
        assert_eq!(without, [] as [&str; 0]);
        // A note is explained with its error.
        let notes = Code::ALL
            .iter()
            .filter(|code| code.severity() == Severity::Note);
        assert!(notes.into_iter().all(|&code| explain(code).is_none()));
    }

    #[test]
    fn a_text_is_whole() {
        for &code in Code::ALL {
            let Some(text) = explain(code) else { continue };
            for part in [text.meaning, text.fix, text.example] {
                assert!(!part.trim().is_empty() && part == part.trim(), "{code}");
            }
            assert!(
                text.meaning.ends_with('.') && text.fix.ends_with('.'),
                "{code}"
            );
        }
    }

    #[test]
    fn a_retired_row_is_a_row_that_nothing_is_reported_with() {
        let lock = LockRegistry::built_in();
        assert!(!lock.is_empty());
        for name in RETIRED {
            let number = lock
                .number(name)
                .unwrap_or_else(|| panic!("{name} is no row"));
            assert_eq!(status(&lock, number), Some(Status::Retired), "{name}");
        }
        assert_eq!(status(&lock, 0), None);
        assert_eq!(status(&lock, lock.len() as u16 + 1), None);
        let undefined = lock.number("err_undefined").unwrap();
        assert_eq!(
            status(&lock, undefined),
            Some(Status::Reported(Code::Undefined))
        );
        let note = lock.number("note_have").unwrap();
        assert_eq!(status(&lock, note), Some(Status::Note(Code::NoteHave)));
        let reserved = lock.number("err_use_after_move").unwrap();
        assert_eq!(status(&lock, reserved), Some(Status::Reserved));
    }

    #[test]
    fn a_code_is_explained_by_its_number_or_its_name() {
        let expected = "E0450 err_inline_marks_conflict\n\nA function is marked `@Inline` and \
                        `@NoInline`.\n\nFix: Keep one of them.\n\nExample:\n\n    @Inline\n    \
                        @NoInline\n    func f() {} // error\n";
        for query in ["E0450", "e0450", "450", "0450", "err_inline_marks_conflict"] {
            assert_eq!(explained(query).as_deref(), Ok(expected), "{query}");
        }
        let note = "E0455 note_have\n\nA note: it is printed under the error that it belongs to.\n";
        assert_eq!(explained("E0455").as_deref(), Ok(note));
        let reserved = "E0451 err_use_after_move\n\nReserved: this version of gopp reports \
                        nothing with this code.\n";
        assert_eq!(explained("E0451").as_deref(), Ok(reserved));
        let retired = "E0421 err_unit_in_func\n\nRetired: the language no longer has what this \
                       code was for.\n";
        assert_eq!(explained("E0421").as_deref(), Ok(retired));
        for query in ["E0000", "E9999", "x", "", "err_nothing"] {
            let said = format!("{query} is no error code; gopp explain --list lists them");
            assert_eq!(explained(query), Err(said));
        }
    }

    #[test]
    fn every_row_is_listed() {
        let lock = LockRegistry::built_in();
        let listed = listed();
        assert_eq!(listed.lines().count(), lock.len());
        assert!(listed.starts_with("E0001 err_too_many_errors\nE0002 err_lex_illegal_nul\n"));
        assert!(listed.contains("\nE0421 err_unit_in_func (retired)\n"));
        assert!(listed.contains("\nE0451 err_use_after_move (reserved)\n"));
    }
}
