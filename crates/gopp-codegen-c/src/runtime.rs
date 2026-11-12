//! The runtime that every program is linked with (ADR-0035): what
//! generated code and the runtime both declare, and how the runtime is
//! built for a target.

use std::fmt::Write;
use std::fs;
use std::path::{Path, PathBuf};

use gopp_target::{HEADERS, ObjectFormat, Os, Part, TargetLayout};

use crate::driver::{ClangDriver, DriverError};

#[cfg(test)]
mod tests;

/// A function of the runtime that generated code calls.
#[derive(Debug)]
pub struct RuntimeFunc {
    pub name: &'static str,
    /// Its result and its parameters as C declares them.
    pub result: &'static str,
    pub params: &'static str,
    pub noreturn: bool,
}

/// A function that gives a value back.
const fn gives(result: &'static str, name: &'static str, params: &'static str) -> RuntimeFunc {
    RuntimeFunc {
        name,
        result,
        params,
        noreturn: false,
    }
}

const fn returns(name: &'static str, params: &'static str) -> RuntimeFunc {
    gives("void", name, params)
}

const fn ends(name: &'static str, params: &'static str) -> RuntimeFunc {
    RuntimeFunc {
        name,
        result: "void",
        params,
        noreturn: true,
    }
}

/// Every function of the runtime that generated code may call: the
/// header declares them, and the runtime defines them.
pub const FUNCS: [RuntimeFunc; 37] = [
    returns("_gopp_rt_init", "int argc, char **argv"),
    ends("_gopp_rt_exit", "int code"),
    returns("_gopp_rt_funcs", "gopp_func_table *table"),
    returns("_gopp_print_bool", "bool value"),
    returns("_gopp_print_int", "int64_t value"),
    returns("_gopp_print_uint", "uint64_t value"),
    returns("_gopp_print_float", "double value"),
    returns("_gopp_print_string", "gopp_string value"),
    returns("_gopp_print_pointer", "uint8_t *value"),
    returns("_gopp_print_space", "void"),
    returns("_gopp_print_newline", "void"),
    gives(
        "gopp_string",
        "_gopp_string_concat",
        "gopp_string a, gopp_string b",
    ),
    gives("bool", "_gopp_string_eq", "gopp_string a, gopp_string b"),
    gives(
        "int64_t",
        "_gopp_string_compare",
        "gopp_string a, gopp_string b",
    ),
    gives("gopp_string", "_gopp_string_from_rune", "int64_t rune"),
    gives(
        "uint64_t",
        "_gopp_string_decode",
        "gopp_string text, uint64_t at",
    ),
    gives(
        "gopp_string",
        "_gopp_string_from_bytes",
        "const void *bytes",
    ),
    returns("_gopp_bytes_from_string", "void *slice, gopp_string text"),
    returns(
        "_gopp_slice_make",
        "void *slice, int64_t len, int64_t cap, uint64_t size",
    ),
    returns(
        "_gopp_slice_grow",
        "void *slice, uint64_t more, uint64_t size, bool no_pointers",
    ),
    returns(
        "_gopp_slice_append",
        "void *slice, const void *from, uint64_t size, bool no_pointers",
    ),
    returns("_gopp_slice_copy", "void *slice, uint64_t size"),
    gives(
        "uint64_t",
        "_gopp_slice_copy_into",
        "const void *to, const void *from, uint64_t size",
    ),
    returns("_gopp_slice_clear", "const void *slice, uint64_t size"),
    ends(
        "_gopp_panic_bounds",
        "uint64_t index, uint64_t length, bool is_signed",
    ),
    ends(
        "_gopp_panic_slice_alen",
        "uint64_t index, uint64_t length, bool is_signed",
    ),
    ends(
        "_gopp_panic_slice_acap",
        "uint64_t index, uint64_t capacity, bool is_signed",
    ),
    ends(
        "_gopp_panic_slice_b",
        "uint64_t low, uint64_t high, bool is_signed",
    ),
    ends(
        "_gopp_panic_slice3_alen",
        "uint64_t index, uint64_t length, bool is_signed",
    ),
    ends(
        "_gopp_panic_slice3_acap",
        "uint64_t index, uint64_t capacity, bool is_signed",
    ),
    ends(
        "_gopp_panic_slice3_b",
        "uint64_t high, uint64_t max, bool is_signed",
    ),
    ends(
        "_gopp_panic_slice3_c",
        "uint64_t low, uint64_t high, bool is_signed",
    ),
    ends(
        "_gopp_panic_slice_convert",
        "uint64_t wanted, uint64_t length, bool is_signed",
    ),
    ends("_gopp_panic_nil", "void"),
    ends("_gopp_panic_divide", "void"),
    ends("_gopp_panic_shift", "void"),
    ends(
        "_gopp_panic_must",
        "gopp_string expr, gopp_string position, gopp_string message",
    ),
];

/// Whether generated code may call a function of the name.
pub fn is_func(name: &str) -> bool {
    FUNCS.iter().any(|func| func.name == name)
}

/// The names of the Go++ functions of a unit, which a trace reads: a
/// unit has one table, and the start of the program gives it to the
/// runtime.
const TABLES: &str = "
typedef struct gopp_func_name {
  void *pc;
  const char *name;
} gopp_func_name;

typedef struct gopp_func_table {
  const gopp_func_name *funcs;
  uint64_t count;
  struct gopp_func_table *next;
} gopp_func_table;
";

/// The names of the types that C has for the scalars, without a header
/// of the C library: Clang knows them for every target.
const SCALARS: &str = "\
typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __UINTPTR_TYPE__ uintptr_t;
";

/// `gopp_rt.h`: what the runtime includes and what every unit starts
/// with. It names no header, so it compiles for any target; C checks
/// that it lays the headers out as the target does.
pub fn header(layout: TargetLayout) -> String {
    let mut out = SCALARS.to_owned();
    let pointer = layout.pointer().size;
    let _ = writeln!(
        out,
        "\n_Static_assert(sizeof(void *) == {pointer}, \"the width of a pointer\");\n\
         _Static_assert(_Alignof(int64_t) == 8, \"the alignment of int64\");"
    );
    for header in HEADERS {
        let name = header.name;
        let _ = writeln!(out, "\ntypedef struct {name} {{");
        for (field, part) in header.fields {
            let ty = match part {
                Part::Bytes => "uint8_t *",
                Part::Elems => "void *",
                Part::U64 => "uint64_t ",
            };
            let _ = writeln!(out, "  {ty}{field};");
        }
        let _ = writeln!(out, "}} {name};");
        let (whole, offsets) = layout.header(header);
        let _ = writeln!(
            out,
            "_Static_assert(sizeof({name}) == {} && _Alignof({name}) == {}, \"the layout of {name}\");",
            whole.size, whole.align
        );
        for ((field, _), offset) in header.fields.iter().zip(offsets) {
            let _ = writeln!(
                out,
                "_Static_assert(__builtin_offsetof({name}, {field}) == {offset}, \"{name}.{field}\");"
            );
        }
    }
    out.push_str(TABLES);
    out.push('\n');
    for func in &FUNCS {
        let noreturn = match func.noreturn {
            true => "[[noreturn]] ",
            false => "",
        };
        let (result, name) = (func.result, func.name);
        let _ = writeln!(out, "{noreturn}{result} {name}({});", func.params);
    }
    out
}

const HEADER_NAME: &str = "gopp_rt.h";

/// A file of the runtime with its text, which `gopp` carries.
type Source = (&'static str, &'static str);

const INTERNAL: Source = ("internal.h", include_str!("../../../runtime/internal.h"));

/// The sources of the runtime for a system: every target has all but
/// the last.
fn sources(os: Os) -> [Source; 6] {
    let of_system = match os {
        Os::Windows => (
            "os_windows.c",
            include_str!("../../../runtime/os_windows.c"),
        ),
        _ => ("os_linux.c", include_str!("../../../runtime/os_linux.c")),
    };
    [
        ("rt.c", include_str!("../../../runtime/rt.c")),
        ("print.c", include_str!("../../../runtime/print.c")),
        ("panic.c", include_str!("../../../runtime/panic.c")),
        ("string.c", include_str!("../../../runtime/string.c")),
        ("slice.c", include_str!("../../../runtime/slice.c")),
        of_system,
    ]
}

/// What the runtime is built as.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Form {
    /// Objects that an executable is linked with.
    Objects,
    /// One shared object, which the programs of the test runner load.
    Shared,
}

const SHARED_NAME: &str = "libgopp_rt.so";

/// The runtime for the target and the flags of `driver`, built once
/// and kept under `cache`: the files that a link is given.
pub fn build(driver: &ClangDriver, form: Form, cache: &Path) -> Result<Vec<PathBuf>, DriverError> {
    // Our own C has no warning.
    let driver = ClangDriver {
        strict: true,
        ..*driver
    };
    if form == Form::Shared {
        if driver.target.object != ObjectFormat::Elf {
            let target = driver.target.triple;
            return Err(DriverError::NoSharedRuntime { target });
        }
        driver.check_link_inputs()?;
    }
    let header = header(driver.target.layout());
    let sources = sources(driver.target.os);
    let made: Vec<String> = match form {
        Form::Objects => (sources.iter())
            .map(|(name, _)| Path::new(name).with_extension("o").display().to_string())
            .collect(),
        Form::Shared => vec![SHARED_NAME.to_owned()],
    };
    let dir = kept(cache, &key(&driver, form, &header, &sources), |dir| {
        let write = |name: &str, text: &str| {
            let path = dir.join(name);
            fs::write(&path, text).map_err(|source| DriverError::Write { path, source })
        };
        write(HEADER_NAME, &header)?;
        for (name, text) in sources.iter().chain([&INTERNAL]) {
            write(name, text)?;
        }
        let mut command = driver.clang(dir);
        command.args(driver.compile_flags(dir));
        command.arg("-Wextra");
        match form {
            Form::Objects => drop(command.arg("-c")),
            Form::Shared => {
                command.args(["-fPIC", "-shared"]);
                command.args(driver.link_flags());
                command.args(["-o", SHARED_NAME]);
            }
        }
        command.args(sources.iter().map(|(name, _)| name));
        driver.run(command).map(drop)
    })?;
    Ok(made.iter().map(|name| dir.join(name)).collect())
}

/// Everything that the files of a build are a function of.
fn key(driver: &ClangDriver, form: Form, header: &str, sources: &[Source]) -> String {
    let clang = &driver.toolchain.clang;
    let mut key = format!("{form:?}\n{} {}\n", clang.path.display(), clang.version);
    // The directory of a build is no part of what it makes.
    for flag in driver.compile_flags(Path::new(".")) {
        let _ = writeln!(key, "{flag}");
    }
    let _ = writeln!(key, "--- {HEADER_NAME}\n{header}");
    for (name, text) in sources.iter().chain([&INTERNAL]) {
        let _ = writeln!(key, "--- {name}\n{text}");
    }
    key
}

const KEY_NAME: &str = "key";

/// The directory under `cache` with what `make` makes for the key. It
/// is made in a directory of its own and then given its name, so that
/// a directory with its name is whole, whoever made it.
fn kept(
    cache: &Path,
    key: &str,
    make: impl FnOnce(&Path) -> Result<(), DriverError>,
) -> Result<PathBuf, DriverError> {
    let failed = |path: &Path| {
        let path = path.to_owned();
        move |source| DriverError::Write { path, source }
    };
    let all = cache.join("runtime");
    let dir = all.join(format!("{:016x}", fnv1a(key.as_bytes())));
    let is_kept =
        |dir: &Path| fs::read(dir.join(KEY_NAME)).is_ok_and(|kept| kept == key.as_bytes());
    if is_kept(&dir) {
        return Ok(dir);
    }
    fs::create_dir_all(&all).map_err(failed(&all))?;
    let building = tempfile::tempdir_in(&all).map_err(failed(&all))?;
    make(building.path())?;
    let key_file = building.path().join(KEY_NAME);
    fs::write(&key_file, key).map_err(failed(&key_file))?;
    let building = building.keep();
    if fs::rename(&building, &dir).is_ok() {
        return Ok(dir);
    }
    // Another build gave the directory its name first; one of another
    // key that has the same number makes way.
    if !is_kept(&dir) {
        fs::remove_dir_all(&dir).map_err(failed(&dir))?;
        fs::rename(&building, &dir).map_err(failed(&dir))?;
        return Ok(dir);
    }
    fs::remove_dir_all(&building).map_err(failed(&building))?;
    Ok(dir)
}

/// A number for a text that is the same on every machine: the name of
/// a directory, never a proof that two texts are one.
fn fnv1a(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for &byte in bytes {
        hash = (hash ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}
