//! From a verified module to one unit of GNU C23 (pipelines.md §1,
//! targets-and-linking.md). The unit includes no header: it starts with
//! the text of the header of the runtime, so it compiles for any target
//! without a sysroot. It is the same text for the same module, whatever
//! the number of threads that write it.

mod body;
mod number;
mod types;

#[cfg(test)]
mod tests;

use std::fmt::Write;

use gopp_gir::{Function, Linkage, Module, Type, TypeError, TypeId, verify_module};
use gopp_target::{Layout, TargetLayout};
use rayon::prelude::*;

/// What a unit is written for.
#[derive(Clone, Copy, Debug)]
pub struct EmitOptions {
    pub layout: TargetLayout,
    /// Whether the unit gets `int main(int, char **)`, which runs the
    /// `init` functions of the package and then its `main`.
    pub entry: bool,
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

    /// Whether the global of the number takes no room.
    pub fn empty_global(&self, at: usize) -> bool {
        self.globals.get(at).is_some_and(|&ty| self.empty(ty))
    }
}

/// The name that a function has in the unit; the symbol is its label.
pub(crate) fn func_name(at: usize) -> String {
    format!("gopp_f{at}")
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
    };
    let mut out = crate::runtime::header(options.layout);
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
            "{}{} {} __asm__(\"{}\");",
            linkage(global.link),
            cx.spelled(global.ty),
            global_name(at),
            quoted(global.name.as_bytes())
        );
    }

    out.push('\n');
    let funcs: Vec<&Function> = module.funcs.values().collect();
    for (at, func) in funcs.iter().enumerate() {
        steps += 1;
        let _ = writeln!(out, "{};", body::prototype(&cx, at, func, true)?);
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
    if options.entry {
        entry(&cx, &funcs, &mut out);
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

/// `main` of C: the runtime is told that the program starts, then the
/// `init` functions of the package run in their order, then its `main`.
/// A package without a `main` gets none.
fn entry(cx: &Cx<'_>, funcs: &[&Function], out: &mut String) {
    let package = &cx.module.name;
    let defined = |name: &str| {
        let found = funcs.iter().position(|func| func.name() == name);
        found.filter(|&at| !funcs[at].is_declaration())
    };
    let Some(main) = defined(&gopp_mangle::func_symbol(package, "main")) else {
        return;
    };
    out.push_str("\nint main(int argc, char **argv) {\n  _gopp_rt_init(argc, argv);\n");
    let mut number = 0;
    while let Some(init) = defined(&gopp_mangle::init_symbol(package, number)) {
        let _ = writeln!(out, "  {}();", func_name(init));
        number += 1;
    }
    let _ = writeln!(out, "  {}();\n  return 0;\n}}", func_name(main));
}

/// What a unit has after the header of the runtime and before its own
/// helpers.
const PRELUDE: &str = "
[[maybe_unused]] static char gopp_empty;

[[maybe_unused]] static inline float gopp_f32(uint32_t bits) {
  union { uint32_t bits; float value; } of = {.bits = bits};
  return of.value;
}
[[maybe_unused]] static inline double gopp_f64(uint64_t bits) {
  union { uint64_t bits; double value; } of = {.bits = bits};
  return of.value;
}
";
