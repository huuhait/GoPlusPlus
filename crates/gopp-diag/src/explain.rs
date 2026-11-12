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
const RETIRED: [&str; 38] = [
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

    /// The errors and warnings without a text may only become fewer.
    const WITHOUT_TEXT_AT_MOST: usize = 178;

    #[test]
    fn a_code_has_a_text_or_is_counted() {
        let reported = |code: &&Code| code.severity() != Severity::Note;
        let without = Code::ALL
            .iter()
            .filter(reported)
            .filter(|&&code| explain(code).is_none());
        let without: Vec<&str> = without.map(|code| code.name()).collect();
        assert!(
            without.len() <= WITHOUT_TEXT_AT_MOST,
            "{} codes have no text, {WITHOUT_TEXT_AT_MOST} had none before",
            without.len()
        );
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
