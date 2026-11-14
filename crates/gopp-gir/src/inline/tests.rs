use gopp_testutil::linear::assert_near_linear;

use super::*;
use crate::lower::tests::module_of;
use crate::print::print_module;
use crate::verify::verify_module;

/// The module of the text with the calls of its small functions
/// replaced, as text; it must keep every rule.
fn inlined(text: &str) -> String {
    let mut module = module_of(text);
    inline_trivial(&mut module);
    assert_eq!(verify_module(&module), Ok(()), "{}", print_module(&module));
    print_module(&module)
}

/// The function of the symbol in the text of a module.
fn function<'t>(module: &'t str, symbol: &str) -> &'t str {
    let start = module.find(&format!("\nfn {symbol}(")).unwrap();
    let rest = &module[start + 1..];
    &rest[..rest.find("\n}\n").unwrap()]
}

const SMALL: &str = "package main

type Vec struct {
	x, y float64
}

func (v Vec) X() float64 { return v.x }

func (v *Vec) SetX(x float64) { v.x = x }

func add(a, b int) int { return a + b }

func dot(a, b Vec) float64 { return a.x*b.x + a.y*b.y }

func nothing() {}

func main() {
	v := Vec{1, 2}
	v.SetX(3)
	nothing()
	println(v.X(), add(1, 2), dot(v, v))
}
";

#[test]
fn a_call_of_a_small_function_is_replaced_by_its_body() {
    let module = inlined(SMALL);
    let main = function(&module, "_GPF4mainE4main");
    for called in [
        "_GPM1XN4mainE3Vec",
        "_GPM4SetXN4mainE3Vec",
        "_GPF4mainE3add",
        "_GPF4mainE3dot",
    ] {
        assert!(
            !main.contains(&format!("call const fn @{called}(")),
            "{called}\n{main}"
        );
        // The function itself stays: another package may call it.
        assert!(module.contains(&format!("\nfn {called}(")), "{called}");
        // Its body is a scope of its own, where the call was written.
        assert!(
            main.contains(&format!("inlined @{called} in ")),
            "{called}\n{main}"
        );
    }
    // Each argument and each result crosses by a copy that is marked;
    // a function that takes and gives nothing has none.
    let crossing = main
        .lines()
        .filter(|line| line.trim_start().starts_with("boundary "));
    assert_eq!(crossing.count(), 2 + 2 + 3 + 3, "{main}");
    assert!(main.contains("inlined @_GPF4mainE7nothing in "), "{main}");
    // The sum of `add` is worked out in `main` now.
    assert!(main.contains(" = add(copy _"), "{main}");
}

#[test]
fn what_is_not_small_is_still_called() {
    let call = |decl: &str, arg: &str| {
        let text = format!(
            "package main\n\nfunc g(x int) int {{ return x * 2 }}\n\n{decl}\n\n\
             func main() {{ println(f({arg})) }}\n"
        );
        let module = inlined(&text);
        function(&module, "_GPF4mainE4main").contains("call const fn @_GPF4mainE1f(")
    };
    assert!(call("func f(x int) int { return g(x) + 1 }", "1"));
    assert!(call("func f(xs []int) int { return xs[0] }", "[]int{1}"));
    assert!(call(
        "func f(x int) int {\n\tif x > 0 {\n\t\treturn 1\n\t}\n\treturn 2\n}",
        "1"
    ));
    assert!(call("func f(s string) int { return len(s + s) }", "\"a\""));
    assert!(call("@NoInline\nfunc f(x int) int { return x + 1 }", "1"));
    assert!(!call("func f(x int) int { return x + 1 }", "1"));
    // More statements than a small function has, unless it is marked.
    let long = format!(
        "func f(x int) int {{\n{}\treturn x\n}}",
        "\tx = x*3 + 1\n".repeat(MAX_STATEMENTS)
    );
    assert!(call(&long, "1"));
    assert!(!call(&format!("@Inline\n{long}"), "1"));
}

#[test]
fn a_call_that_a_cleanup_follows_is_replaced_and_the_cleanup_goes_with_it() {
    // `joined` is ended if a panic leaves the call of `add`, which is
    // the one call of `f` that a cleanup follows.
    let text = "package main

func add(a, b int) int { return a + b }

func f(s string) int {
	joined := s + s
	n := add(1, 2)
	return n + len(joined)
}

func main() { println(f(\"a\")) }
";
    let before = print_module(&module_of(text));
    let f = function(&before, "_GPF4mainE1f");
    let cleaned: Vec<&str> = (f.lines())
        .filter(|line| line.contains("unwind: cleanup bb"))
        .collect();
    assert!(
        cleaned.len() == 1 && cleaned[0].contains("@_GPF4mainE3add("),
        "{f}"
    );
    assert!(f.contains(": [cleanup]"), "{f}");
    // A small function does not unwind: its body needs no cleanup, and
    // the blocks that nothing leads to any more are gone.
    let after = inlined(text);
    let f = function(&after, "_GPF4mainE1f");
    assert!(
        !f.contains("@_GPF4mainE3add(") && !f.contains(": [cleanup]"),
        "{f}"
    );
    assert!(f.contains("inlined @_GPF4mainE3add in "), "{f}");
}

#[test]
fn the_steps_grow_with_the_calls_and_with_the_functions() {
    let steps = |text: &str| inline_trivial(&mut module_of(text));
    let sizes = [100, 200, 400, 800];
    // Many calls of one small function in one function.
    assert_near_linear(&sizes, |n| {
        let calls = "\tx = add(x, 1)\n".repeat(n);
        steps(&format!(
            "package main\n\nfunc add(a, b int) int {{ return a + b }}\n\n\
             func f(x int) int {{\n{calls}\treturn x\n}}\n"
        ))
    });
    // Many small functions, each called once by the next that is not.
    assert_near_linear(&sizes, |n| {
        let funcs: String = (0..n)
            .map(|at| {
                format!(
                    "func s{at}(a int) int {{ return a + {at} }}\n\n\
                     func c{at}(xs []int) int {{ return s{at}(xs[0]) }}\n\n"
                )
            })
            .collect();
        steps(&format!("package main\n\n{funcs}"))
    });
}
