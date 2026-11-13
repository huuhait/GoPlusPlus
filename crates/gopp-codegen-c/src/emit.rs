//! From a verified module to one unit of GNU C23 (pipelines.md §1,
//! targets-and-linking.md). The unit includes no header: it starts with
//! the text of the header of the runtime, so it compiles for any target
//! without a sysroot. It is the same text for the same module, whatever
//! the number of threads that write it.

mod body;
mod data;
mod number;
mod types;

#[cfg(test)]
mod tests;

use std::fmt::Write;

use gopp_gir::{
    CallConv, FuncType, Function, Global, Linkage, Module, Type, TypeError, TypeId, verify_module,
};
use gopp_target::{Layout, TargetLayout};
use rayon::prelude::*;

/// The function of C that starts the program of a unit: it runs the
/// `init` functions of the package and then its `main`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Entry {
    /// None: the unit is no program.
    None,
    /// `int main(int, char **)`, for an executable.
    Main,
    /// `int gopp_entry(void)`, for a shared module that the test runner
    /// loads.
    Module,
}

/// What a unit is written for.
#[derive(Clone, Copy, Debug)]
pub struct EmitOptions {
    pub layout: TargetLayout,
    pub entry: Entry,
    /// Whether Clang has `swiftcall` for the target: the convention of
    /// the Go++ functions then (ADR-0030).
    pub swiftcall: bool,
}

#[derive(Debug, thiserror::Error)]
pub enum EmitError {
    #[error("the module breaks a rule: {0}")]
    Verify(String),
    #[error("{0}")]
    Type(#[from] TypeError),
    #[error("not supported yet{}: {what}", within(.func))]
    Unsupported {
        func: Option<String>,
        what: &'static str,
    },
}

fn within(func: &Option<String>) -> String {
    func.as_ref()
        .map_or(String::new(), |func| format!(" in {func}"))
}

/// The module as one unit of C.
pub fn emit_c23(module: &Module, options: &EmitOptions) -> Result<String, EmitError> {
    emit_counted(module, options).map(|(text, _)| text)
}

/// What every part of the unit is written with.
pub(crate) struct Cx<'m> {
    pub module: &'m Module,
    /// The layout of each type, and how each is written in C where a
    /// value of it is declared.
    layouts: Vec<Layout>,
    spelled: Vec<String>,
    /// The types of the globals, by their numbers.
    globals: Vec<TypeId>,
    /// The name that each function has in the unit.
    funcs: Vec<String>,
}

impl Cx<'_> {
    /// Whether a value of the type takes no room: nothing is written
    /// for it.
    pub fn empty(&self, ty: TypeId) -> bool {
        self.layouts[ty.index()].size == 0
    }

    pub fn layout(&self, ty: TypeId) -> Layout {
        self.layouts[ty.index()]
    }

    pub fn spelled(&self, ty: TypeId) -> &str {
        &self.spelled[ty.index()]
    }

    pub fn kind(&self, ty: TypeId) -> &Type {
        self.module.types.get(ty)
    }

    /// Whether a function of the type is called by `GOPP_CALL`. A Go++
    /// function is, unless a parameter or its result has 3, 5, 6 or 7
    /// bytes after its whole words: Clang 23 passes such a value by
    /// `swiftcall` in whole words and writes the last of them past the
    /// end of the value. Such a function is called as a function of C,
    /// for every target alike.
    pub fn go_call(&self, func: &FuncType) -> bool {
        let whole = |&ty: &TypeId| !matches!(self.layout(ty).size % 8, 3 | 5 | 6 | 7);
        func.cc == CallConv::Go && func.params.iter().chain([&func.ret]).all(whole)
    }

    /// The name that the function of the number has in the unit: the
    /// symbol is its label. A function of the runtime has its own name,
    /// which the header declares.
    pub fn func_name(&self, at: usize) -> &str {
        &self.funcs[at]
    }

    /// Whether the global of the number takes no room.
    pub fn empty_global(&self, at: usize) -> bool {
        self.globals.get(at).is_some_and(|&ty| self.empty(ty))
    }
}

/// What the name of every function of the runtime starts with.
const RUNTIME_PREFIX: &str = "_gopp_";

/// The name of each function in the unit. A module that names a
/// function of the runtime that the runtime does not have is refused.
fn func_names(module: &Module) -> Result<Vec<String>, EmitError> {
    let named = |(at, func): (usize, &Function)| {
        let name = func.name();
        match func.is_declaration() && name.starts_with(RUNTIME_PREFIX) {
            true if crate::runtime::is_func(name) => Ok(name.to_owned()),
            true => Err(EmitError::Unsupported {
                func: Some(name.to_owned()),
                what: "a function that the runtime does not have",
            }),
            false => Ok(format!("gopp_f{at}")),
        }
    };
    module.funcs.values().enumerate().map(named).collect()
}

pub(crate) fn global_name(at: usize) -> String {
    format!("gopp_g{at}")
}

/// `text` as the inside of a string literal of C.
pub(crate) fn quoted(text: &[u8]) -> String {
    let mut out = String::with_capacity(text.len());
    for &byte in text {
        match byte {
            b'"' | b'\\' => {
                out.push('\\');
                out.push(byte as char);
            }
            // Three digits: a digit that follows is no part of it.
            b' '..=b'~' if byte != b'?' => out.push(byte as char),
            _ => {
                let _ = write!(out, "\\{byte:03o}");
            }
        }
    }
    out
}

/// `emit_c23`, with the steps that it took.
pub(crate) fn emit_counted(
    module: &Module,
    options: &EmitOptions,
) -> Result<(String, u64), EmitError> {
    if let Err(errors) = verify_module(module) {
        let first: Vec<String> = errors.iter().take(4).map(ToString::to_string).collect();
        return Err(EmitError::Verify(first.join("; ")));
    }
    let layouts = module.types.layouts(options.layout)?;
    let mut steps = 0;
    let spelled = types::spelled(module, &layouts, &mut steps);
    let cx = Cx {
        module,
        layouts,
        spelled,
        globals: module.globals.values().map(|global| global.ty).collect(),
        funcs: func_names(module)?,
    };
    let mut out = crate::runtime::header(options.layout);
    out.push_str(convention(options.swiftcall));
    out.push_str(PRELUDE);
    number::helpers(&mut out);
    types::definitions(&cx, &mut out, &mut steps)?;

    out.push('\n');
    for (at, text) in module.strings.iter().enumerate() {
        steps += 1;
        let _ = writeln!(
            out,
            "static const char gopp_s{at}[] = \"{}\";",
            quoted(text)
        );
    }
    for (at, (_, global)) in module.globals.iter().enumerate() {
        steps += 1;
        if cx.empty(global.ty) {
            continue;
        }
        let _ = writeln!(
            out,
            "{}{}{} {} __asm__(\"{}\");",
            linkage(global.link),
            constness(global),
            cx.spelled(global.ty),
            global_name(at),
            quoted(global.name.as_bytes())
        );
    }

    out.push('\n');
    let funcs: Vec<&Function> = module.funcs.values().collect();
    for (at, func) in funcs.iter().enumerate() {
        steps += 1;
        if cx.func_name(at).starts_with(RUNTIME_PREFIX) {
            continue;
        }
        let _ = writeln!(out, "{};", body::prototype(&cx, at, func, true)?);
    }
    // Static data names functions, so it comes after their prototypes.
    for (at, (_, global)) in module.globals.iter().enumerate() {
        if !global.parts.is_empty() && !cx.empty(global.ty) {
            data::global(&cx, at, global, &mut out, &mut steps)?;
        }
    }
    // Each function is written on its own; the unit has them in their
    // order.
    let bodies: Vec<Result<(String, u64), EmitError>> = funcs
        .par_iter()
        .enumerate()
        .map(|(at, func)| match func.is_declaration() {
            true => Ok((String::new(), 0)),
            false => body::function(&cx, at, func),
        })
        .collect();
    for written in bodies {
        let (text, taken) = written?;
        steps += taken;
        out.push_str(&text);
    }
    if options.entry != Entry::None {
        entry(&cx, &funcs, options.entry, &mut out);
    }
    Ok((out, steps))
}

/// What a declaration starts with for its linkage.
pub(crate) fn linkage(link: Linkage) -> &'static str {
    match link {
        Linkage::External => "",
        Linkage::Internal => "static ",
        Linkage::LinkOnceOdr => "__attribute__((weak)) ",
    }
}

/// What the declaration of a global that the program never writes has
/// before its type.
pub(crate) fn constness(global: &Global) -> &'static str {
    match global.mutable {
        true => "",
        false => "const ",
    }
}

/// How long the name of a function in a trace may be.
const MAX_TRACE_NAME: usize = 256;

/// The start of the program: the runtime is told that the program
/// starts and what its functions are called in a trace, then the `init`
/// functions of the package run in their order, then its `main`. A
/// package without a `main` gets none.
fn entry(cx: &Cx<'_>, funcs: &[&Function], kind: Entry, out: &mut String) {
    let package = &cx.module.name;
    let defined = |name: &str| {
        let found = funcs.iter().position(|func| func.name() == name);
        found.filter(|&at| !funcs[at].is_declaration())
    };
    let Some(main) = defined(&gopp_mangle::func_symbol(package, "main")) else {
        return;
    };
    out.push_str("\nstatic const gopp_func_name gopp_funcs[] = {\n");
    let mut count = 0;
    for (at, func) in funcs.iter().enumerate() {
        if func.is_declaration() {
            continue;
        }
        // A function is called what its symbol reads as.
        let symbol = func.name().as_bytes();
        let readable = gopp_mangle::demangle(symbol, MAX_TRACE_NAME);
        let name = readable.unwrap_or_else(|_| func.name().to_owned());
        let name = quoted(name.as_bytes());
        let _ = writeln!(out, "  {{(void *){}, \"{name}\"}},", cx.func_name(at));
        count += 1;
    }
    let _ = writeln!(
        out,
        "}};\nstatic gopp_func_table gopp_table = {{gopp_funcs, {count}, 0}};"
    );
    out.push_str(match kind {
        Entry::Module => "\nint gopp_entry(void) {\n  _gopp_rt_init(0, 0);\n",
        _ => "\nint main(int argc, char **argv) {\n  _gopp_rt_init(argc, argv);\n",
    });
    out.push_str("  _gopp_rt_funcs(&gopp_table);\n");
    let mut number = 0;
    while let Some(init) = defined(&gopp_mangle::init_symbol(package, number)) {
        let _ = writeln!(out, "  {}();", cx.func_name(init));
        number += 1;
    }
    let _ = writeln!(out, "  {}();\n  return 0;\n}}", cx.func_name(main));
}

/// How a Go++ function is called, and how it gets the environment of a
/// closure: by Clang's `swiftcall`, which returns four words in
/// registers and has a register for the environment, or as a function
/// of C where the target has no `swiftcall`.
pub(crate) fn convention(swiftcall: bool) -> &'static str {
    match swiftcall {
        true => {
            "\n#define GOPP_CALL __attribute__((swiftcall))\n\
             #define GOPP_ENV __attribute__((swift_context))\n"
        }
        false => "\n#define GOPP_CALL\n#define GOPP_ENV\n",
    }
}

/// What a unit has after the header of the runtime and before its own
/// helpers.
const PRELUDE: &str = "
[[maybe_unused]] static char gopp_empty;

[[maybe_unused]] static inline float gopp_float32(uint32_t bits) {
  union { uint32_t bits; float value; } of = {.bits = bits};
  return of.value;
}
[[maybe_unused]] static inline double gopp_float64(uint64_t bits) {
  union { uint64_t bits; double value; } of = {.bits = bits};
  return of.value;
}
";
