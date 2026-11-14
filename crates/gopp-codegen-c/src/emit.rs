//! From a verified module to one unit of GNU C23 (pipelines.md §1,
//! targets-and-linking.md). The unit includes no header: it starts with
//! the text of the header of the runtime, so it compiles for any target
//! without a sysroot. It is the same text for the same module, whatever
//! the number of threads that write it.

mod body;
mod data;
mod names;
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
    /// `gopp.start.<import path>`, for a package that a program imports:
    /// the program calls it once, before anything of the package is used.
    Package,
    /// `int main(int, char **)`, for an executable.
    Main,
    /// `int gopp_entry(void)`, for a shared module that the test runner
    /// loads.
    Module,
}

/// What a unit is written for.
#[derive(Clone, Copy, Debug)]
pub struct EmitOptions<'a> {
    pub layout: TargetLayout,
    pub entry: Entry,
    /// The import paths of the packages that the program needs, each
    /// after those that it imports: the program starts them in this
    /// order before its own package (INI-1).
    pub imports: &'a [String],
    /// Whether Clang has `swiftcall` for the target: the convention of
    /// the Go++ functions then (ADR-0030).
    pub swiftcall: bool,
    /// A panic ends the program at once (`-panic=abort`): the program
    /// tells the runtime when it starts.
    pub aborts: bool,
    /// The unit says where in the source each statement is written, for
    /// a debugger (debug-info.md).
    pub debug: bool,
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
pub fn emit_c23(module: &Module, options: &EmitOptions<'_>) -> Result<String, EmitError> {
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
    debug: bool,
    /// For a debugger: the names of the fields of each type, where
    /// they are those of the source.
    fields: Vec<Vec<String>>,
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

    /// The directive that says that what follows is written at the
    /// place of the source: none without debug information, and for
    /// what has no place there.
    pub fn line(&self, loc: gopp_gir::Span) -> Option<String> {
        let file = (loc.file as usize).checked_sub(1)?;
        let file = self.module.files.get(file).filter(|_| self.debug)?;
        let line = loc.line.max(1);
        Some(format!("#line {line} \"{}\"\n", quoted(file.as_bytes())))
    }

    /// The name of the field of the number of a struct or a tuple.
    pub fn field(&self, ty: TypeId, at: usize) -> String {
        let named = self.fields.get(ty.index()).and_then(|names| names.get(at));
        named.map_or_else(|| format!("f{at}"), String::clone)
    }

    /// The names that the fields have from the source, each once.
    pub fn field_names(&self) -> Vec<&str> {
        let numbered = |name: &str| {
            (name.strip_prefix('f')).is_some_and(|number| number.parse::<u32>().is_ok())
        };
        let mut names: Vec<&str> = (self.fields.iter().flatten())
            .map(|name| &**name)
            .filter(|name| !numbered(name))
            .collect();
        names.sort_unstable();
        names.dedup();
        names
    }

    /// Whether the unit says where its statements are in the source.
    pub fn line_tables(&self) -> bool {
        self.debug
    }

    /// Whether the name is that of a function of the runtime.
    pub fn of_runtime(&self, name: &str) -> bool {
        name.starts_with(RUNTIME_PREFIX)
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
/// For a debugger a function of the source has the name that a trace
/// writes for it, as a name of C (`names::c_name`): `main__add`. A name
/// that a type of the unit has already, or another function, is not
/// given twice.
fn func_names(module: &Module, tags: &[String]) -> Result<Vec<String>, EmitError> {
    let mut given: std::collections::HashSet<&str> = tags.iter().map(|tag| &**tag).collect();
    let debug = !tags.is_empty();
    let readable: Vec<Option<String>> = (module.funcs.values())
        .map(
            |func| match debug && !func.is_declaration() && func.loc().file != 0 {
                true => gopp_mangle::demangle(func.name().as_bytes(), MAX_TRACE_NAME).ok(),
                false => None,
            },
        )
        .map(|readable| readable.and_then(|readable| names::c_name(&readable)))
        .collect();
    let mut named = Vec::with_capacity(readable.len());
    for (at, (func, readable)) in module.funcs.values().zip(&readable).enumerate() {
        let name = func.name();
        let own = readable.as_deref().filter(|written| given.insert(written));
        named.push(
            match (
                func.is_declaration() && name.starts_with(RUNTIME_PREFIX),
                own,
            ) {
                (true, _) if crate::runtime::is_func(name) => name.to_owned(),
                (true, _) => {
                    return Err(EmitError::Unsupported {
                        func: Some(name.to_owned()),
                        what: "a function that the runtime does not have",
                    });
                }
                (false, Some(written)) => written.to_owned(),
                (false, None) => format!("gopp_f{at}"),
            },
        );
    }
    Ok(named)
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
    let (tags, fields) = match options.debug {
        true => names::of_types(module, &mut steps),
        false => (Vec::new(), Vec::new()),
    };
    let spelled = types::spelled(module, &layouts, &tags, &mut steps);
    let cx = Cx {
        module,
        layouts,
        spelled,
        globals: module.globals.values().map(|global| global.ty).collect(),
        funcs: func_names(module, &tags)?,
        debug: options.debug,
        fields,
    };
    let mut out = crate::runtime::header(options.layout);
    out.push_str(convention(options.swiftcall));
    out.push_str(PRELUDE);
    number::helpers(&mut out);
    types::definitions(&cx, options.layout, &mut out, &mut steps)?;

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
        // A global that another unit has, or whose data is written
        // further down, is only declared here; one that starts as
        // zeros is what this line makes.
        let later = !global.parts.is_empty() && global.link != Linkage::Internal;
        let start = match !global.defined || later {
            true => "extern ",
            false => linkage(global.link, true),
        };
        let _ = writeln!(
            out,
            "{start}{}{} {} __asm__(\"{}\");",
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
    // What follows is written nowhere in the source.
    if options.debug {
        out.push_str("#line 1 \"gopp-generated\"\n");
    }
    if options.entry != Entry::None {
        entry(&cx, &funcs, options, &mut out);
    }
    Ok((out, steps))
}

/// What a declaration starts with for its linkage. What several units
/// may each have, of which a program keeps one, is said as the system
/// of the target takes it (`GOPP_ONCE`, `GOPP_ONCE_DATA`).
pub(crate) fn linkage(link: Linkage, data: bool) -> &'static str {
    match (link, data) {
        (Linkage::External, _) => "",
        (Linkage::Internal, _) => "static ",
        (Linkage::LinkOnceOdr, false) => "GOPP_ONCE ",
        (Linkage::LinkOnceOdr, true) => "GOPP_ONCE_DATA ",
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

/// What the function that starts the package at the import path is
/// called for the linker.
fn start_symbol(path: &str) -> String {
    format!("gopp.start.{}", quoted(path.as_bytes()))
}

/// The start of the program: the runtime is told that the program
/// starts and what its functions are called in a trace, the packages
/// that it imports are started, then the variables of the package get
/// their first values and its `init` functions run in their order, then
/// its `main`. A program without a `main` gets none. The start of a
/// package that is imported is the part for the package alone.
fn entry(cx: &Cx<'_>, funcs: &[&Function], options: &EmitOptions, out: &mut String) {
    let kind = options.entry;
    let package = &cx.module.name;
    let defined = |name: &str| {
        let found = funcs.iter().position(|func| func.name() == name);
        found.filter(|&at| !funcs[at].is_declaration())
    };
    let main = defined(&gopp_mangle::func_symbol(package, "main"));
    if kind != Entry::Package && main.is_none() {
        return;
    }
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
    if kind == Entry::Package {
        let symbol = start_symbol(package);
        let _ = writeln!(out, "\nvoid gopp_start(void) __asm__(\"{symbol}\");");
        out.push_str("void gopp_start(void) {\n  _gopp_rt_funcs(&gopp_table);\n");
    } else {
        for (at, import) in options.imports.iter().enumerate() {
            let symbol = start_symbol(import);
            let _ = writeln!(out, "void gopp_start_{at}(void) __asm__(\"{symbol}\");");
        }
        // What starts the program keeps its calls at every level, so
        // that the trace of a panic names `main.main`.
        out.push_str("\n__attribute__((noinline, optnone))");
        out.push_str(match kind {
            Entry::Module => "\nint gopp_entry(void) {\n  _gopp_rt_init(0, 0);\n",
            _ => "\nint main(int argc, char **argv) {\n  _gopp_rt_init(argc, argv);\n",
        });
        out.push_str("  _gopp_rt_funcs(&gopp_table);\n");
        if options.aborts {
            out.push_str("  _gopp_panic_aborts();\n");
        }
        for at in 0..options.imports.len() {
            let _ = writeln!(out, "  gopp_start_{at}();");
        }
    }
    // The variables of the package get their first values, then its
    // `init` functions run (INI-1).
    if let Some(start) = defined(&gopp_mangle::pkg_init_symbol(package)) {
        let _ = writeln!(out, "  {}();", cx.func_name(start));
    }
    let mut number = 0;
    while let Some(init) = defined(&gopp_mangle::init_symbol(package, number)) {
        let _ = writeln!(out, "  {}();", cx.func_name(init));
        number += 1;
    }
    match main.filter(|_| kind != Entry::Package) {
        Some(main) => drop(writeln!(
            out,
            "  {}();\n  return 0;\n}}",
            cx.func_name(main)
        )),
        None => out.push_str("}\n"),
    }
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
// A function or a table that several units have, of which the linker
// keeps one: weak where objects are ELF, and what a linker of COFF
// takes for the same, which refuses two weak definitions.
#ifdef _WIN32
#define GOPP_ONCE inline
#define GOPP_ONCE_DATA __declspec(selectany)
#else
#define GOPP_ONCE __attribute__((weak))
#define GOPP_ONCE_DATA __attribute__((weak))
#endif

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
