//! The runtime that every program is linked with (ADR-0035): what
//! generated code and the runtime both declare, and how the runtime is
//! built for a target.

use std::fmt::Write;
use std::fs;
use std::path::{Path, PathBuf};

use gopp_target::{HEADERS, ObjectFormat, Os, Part, TargetLayout, TargetSpec};

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
pub const FUNCS: [RuntimeFunc; 105] = [
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
    returns("_gopp_string_append", "gopp_string *to, gopp_string more"),
    returns("_gopp_string_retain", "gopp_string text"),
    returns("_gopp_string_release", "gopp_string text"),
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
    gives(
        "gopp_string",
        "_gopp_string_from_runes",
        "const void *runes",
    ),
    returns("_gopp_bytes_from_string", "void *slice, gopp_string text"),
    returns("_gopp_runes_from_string", "void *slice, gopp_string text"),
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
    returns("_gopp_slice_free", "const void *slice"),
    gives(
        "uint64_t",
        "_gopp_slice_move_into",
        "const void *to, const void *from, uint64_t size, void (*drop)(uint8_t *value)",
    ),
    returns(
        "_gopp_each",
        "void *first, uint64_t count, uint64_t size, void (*each)(uint8_t *value)",
    ),
    gives(
        "uint64_t",
        "_gopp_hash_bytes",
        "const void *first, uint64_t count, uint64_t seed",
    ),
    gives(
        "uint64_t",
        "_gopp_hash_string",
        "gopp_string text, uint64_t seed",
    ),
    gives(
        "uint64_t",
        "_gopp_hash_float",
        "double value, uint64_t seed",
    ),
    gives(
        "uint64_t",
        "_gopp_hash_each",
        "void *first, uint64_t count, uint64_t size, uint64_t seed, \
         uint64_t (*hash)(uint8_t *value, uint64_t seed)",
    ),
    gives(
        "bool",
        "_gopp_mem_eq",
        "const void *a, const void *b, uint64_t size",
    ),
    gives(
        "bool",
        "_gopp_each_eq",
        "void *a, void *b, uint64_t count, uint64_t size, bool (*eq)(uint8_t *a, uint8_t *b)",
    ),
    gives(
        "void *",
        "_gopp_map_make",
        "const void *shape, int64_t hint",
    ),
    gives("uint64_t", "_gopp_map_len", "const void *map"),
    gives(
        "bool",
        "_gopp_map_read",
        "const void *map, const void *shape, const void *key, void *value",
    ),
    gives(
        "void *",
        "_gopp_map_set",
        "void *place_of_map, const void *shape, const void *key",
    ),
    returns(
        "_gopp_map_delete",
        "void *map, const void *shape, const void *key",
    ),
    returns("_gopp_map_clear", "void *map, const void *shape"),
    returns("_gopp_map_drop", "void *map, const void *shape"),
    gives(
        "void *",
        "_gopp_map_copy",
        "const void *map, const void *shape",
    ),
    returns("_gopp_map_iter_start", "void *iter, void *map"),
    gives(
        "void *",
        "_gopp_map_iter_next",
        "void *iter, const void *shape, void *key",
    ),
    returns("_gopp_map_iter_end", "void *iter"),
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
    returns("_gopp_func_clone", "uint8_t *value"),
    returns("_gopp_func_drop", "uint8_t *value"),
    gives("uint8_t *", "_gopp_env_new", "uint64_t cells"),
    gives("uint8_t *", "_gopp_cell_new", "uint64_t size"),
    returns("_gopp_cell_retain", "uint8_t *cell"),
    returns(
        "_gopp_cell_release",
        "uint8_t *cell, void (*drop)(uint8_t *value)",
    ),
    returns(
        "_gopp_extend",
        "uint8_t *list, uint8_t *kept, uint8_t *cell, void (*drop)(uint8_t *value)",
    ),
    returns("_gopp_extended_end", "uint8_t *list"),
    gives(
        "uint8_t *",
        "_gopp_iface_box",
        "const void *value, uint64_t size",
    ),
    returns("_gopp_box_free", "uint8_t *box"),
    gives(
        "uint8_t *",
        "_gopp_shared_new",
        "const void *value, uint64_t size",
    ),
    returns("_gopp_shared_retain", "uint8_t *value"),
    gives("bool", "_gopp_shared_release", "uint8_t *value"),
    returns("_gopp_weak_retain", "uint8_t *value"),
    returns("_gopp_weak_release", "uint8_t *value"),
    gives("uint8_t *", "_gopp_weak_lock", "uint8_t *value"),
    gives(
        "uint8_t *",
        "_gopp_part_of",
        "uint8_t *part, const void *of",
    ),
    gives("bool", "_gopp_whole_end", "uint8_t *part"),
    gives("uint8_t *", "_gopp_whole_copy", "uint8_t *part"),
    returns("_gopp_part_retain", "uint8_t *part"),
    gives("bool", "_gopp_part_release", "uint8_t *part"),
    returns("_gopp_part_weak_retain", "uint8_t *part"),
    returns("_gopp_part_weak_release", "uint8_t *part"),
    gives("uint8_t *", "_gopp_part_lock", "uint8_t *part"),
    ends(
        "_gopp_panic_part",
        "const uint8_t *part, const void *of, gopp_string from",
    ),
    returns("_gopp_iface_clone", "uint8_t *value"),
    returns("_gopp_iface_drop", "uint8_t *value"),
    gives("bool", "_gopp_iface_eq", "uint8_t *a, uint8_t *b"),
    gives(
        "uint64_t",
        "_gopp_iface_hash",
        "uint8_t *value, uint64_t seed",
    ),
    gives(
        "uint8_t *",
        "_gopp_iface_table",
        "const uint8_t *itab, const void *wanted",
    ),
    returns("_gopp_defer_push", "uint8_t *frame"),
    returns("_gopp_defer_pop", "uint8_t *frame"),
    gives("bool", "_gopp_defer_enter", "uint8_t *frame"),
    ends(
        "_gopp_panic",
        "uint8_t *value, gopp_string error_key, gopp_string string_key",
    ),
    returns("_gopp_recover", "uint8_t *value"),
    gives("bool", "_gopp_panicking", ""),
    returns(
        "_gopp_err_trace",
        "uint8_t *itab, uint8_t *data, gopp_string position",
    ),
    returns("_gopp_panic_aborts", ""),
    gives("bool", "_gopp_defer_left", "uint8_t *frame"),
    returns("_gopp_defer_owns", "uint8_t *owned"),
    ends("_gopp_panic_drop", ""),
    ends(
        "_gopp_panic_assert",
        "const uint8_t *itab, const void *wanted, gopp_string iface",
    ),
    ends(
        "_gopp_panic_assert_iface",
        "const uint8_t *itab, const void *wanted",
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

// A method of a type as a table of methods of an interface takes it:
// what the table holds for it, under a key that is the same for the
// method of an interface that it is: its name, the package of a name
// that is not exported, and its type.
typedef struct gopp_method {
  gopp_string key;
  uint8_t *code;
} gopp_method;

// The methods of a type, in the order of the bytes of their keys.
typedef struct gopp_methods {
  uint64_t count;
  gopp_method all[];
} gopp_methods;

// An interface type: its name, and the keys of its methods in the
// order of its tables.
typedef struct gopp_itype {
  gopp_string name;
  uint64_t count;
  gopp_string keys[];
} gopp_itype;

// The descriptor of a type whose values an interface value holds: one
// for a type in a program, so two values are of one type exactly when
// their descriptors are one. `type` points to the descriptor itself. A
// function that is null says that the bytes of a value are all there
// is to copy, end, compare or hash; `methods` is null for a type
// without methods.
typedef struct gopp_type {
  const struct gopp_type *type;
  uint64_t size;
  uint64_t flags;
  void (*clone)(uint8_t *value);
  void (*drop)(uint8_t *value);
  bool (*eq)(uint8_t *a, uint8_t *b);
  uint64_t (*hash)(uint8_t *value, uint64_t seed);
  gopp_string name;
  const gopp_methods *methods;
  uint64_t kind;
} gopp_type;

// What a value of a type is, for what writes the value of a panic.
#define GOPP_KIND_OTHER 0
#define GOPP_KIND_BOOL 1
#define GOPP_KIND_INT 2
#define GOPP_KIND_UINT 3
#define GOPP_KIND_FLOAT 4
#define GOPP_KIND_STRING 5

// A function that has `defer` statements and has not returned: the
// function that makes its deferred calls with the record that it is
// given, and where the function goes on when one of the calls stopped
// a panic.
typedef struct gopp_frame {
  struct gopp_frame *next;
  void (*run)(uint8_t *record);
  uint8_t *record;
  void *mark;
} gopp_frame;

// A variable that a function captured, in an environment: its cell,
// and what ends the variable, or null if its bytes are all there is.
typedef struct gopp_captured {
  uint8_t *cell;
  void (*drop)(uint8_t *value);
} gopp_captured;

// The environment of a function value: how many function values share
// it, and the cells of the variables that the function captured.
typedef struct gopp_env {
  uint64_t count;
  uint64_t cells;
  gopp_captured all[];
} gopp_env;

// The count of an environment in the frame of a function, which is
// never counted or freed.
#define GOPP_ENV_IN_FRAME (~(uint64_t)0)

// The value is the second word of an interface value itself, not in a
// box (TYP-8).
#define GOPP_TYPE_INLINE 1
// Two values of the type are compared with `==`.
#define GOPP_TYPE_COMPARABLE 2
// The type has a name that is written with a value of it.
#define GOPP_TYPE_NAMED 4
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
    out.push_str(ENTER);
    out
}

/// A function with `defer` statements marks where it goes on after a
/// panic in its own frame, which a function that it calls cannot do
/// for it.
const ENTER: &str = "
// The function is in the list from here on, with `at`, a variable of
// its own, as its mark. It is here a second time, with true, when one
// of its deferred calls stopped a panic. Clang must know that the
// function goes on twice from here, or it lets what the second time
// reads share its room on the stack with what the first time writes.
#if defined(_WIN32)
// The mark of the C runtime, whose `longjmp` unwinds the stack to the
// function. Clang's own mark does not bring back the frame pointer of
// a function of Windows. That unwinding runs what the functions
// between have for it, and nothing of the function of the mark: when
// the function goes on, it ends what it owned itself.
typedef struct gopp_mark {
  _Alignas(16) uint64_t part[32];
} gopp_mark;
__attribute__((returns_twice)) int gopp_setjmp(void *mark, void *frame) __asm__(\"_setjmp\");
#define GOPP_MARK_UNWOUND 0
#define _gopp_defer_enter(frame, at) \\
  (_gopp_defer_push(frame), ((gopp_frame *)(frame))->mark = &(at), \\
   gopp_setjmp(&(at), __builtin_frame_address(0)) != 0)
#else
// Clang does not know of `__builtin_setjmp` that the function goes on
// twice from it; it knows it of a function that it calls with the mark.
// The unwinder of the system unwinds the function of the mark like
// those between, and the function goes on from its last cleanup block.
typedef struct gopp_mark {
  void *part[5];
} gopp_mark;
__attribute__((returns_twice)) void _gopp_defer_push(uint8_t *frame);
#define GOPP_MARK_UNWOUND 1
#define _gopp_defer_enter(frame, at) \\
  (_gopp_defer_push(frame), ((gopp_frame *)(frame))->mark = &(at), \\
   __builtin_setjmp((at).part) != 0)
#endif
";

pub(crate) const HEADER_NAME: &str = "gopp_rt.h";

/// `gopp_rt.h` as the sources of the runtime include it: with how a
/// Go++ function is called, because the runtime calls the methods of
/// the value of a panic.
pub(crate) fn own_header(target: &TargetSpec) -> String {
    let mut header = header(target.layout());
    header.push_str(crate::emit::convention(target.swiftcall));
    header
}

/// A file of the runtime with its text, which `gopp` carries.
type Source = (&'static str, &'static str);

pub(crate) const INTERNAL: Source = ("internal.h", include_str!("../../../runtime/internal.h"));

/// The sources of the runtime for a system: every target has all but
/// the last.
pub(crate) fn sources(os: Os) -> [Source; 11] {
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
        ("map.c", include_str!("../../../runtime/map.c")),
        ("iface.c", include_str!("../../../runtime/iface.c")),
        ("func.c", include_str!("../../../runtime/func.c")),
        ("shared.c", include_str!("../../../runtime/shared.c")),
        ("table.c", include_str!("../../../runtime/table.c")),
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
    let header = own_header(driver.target);
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

/// Builds the units of C of one program in `dir` with the runtime from
/// its sources and the address, the leak and the undefined-behaviour
/// sanitizers of Clang: the program that says what a program frees.
pub fn build_sanitized(
    driver: &ClangDriver,
    units: &[String],
    dir: &Path,
) -> Result<PathBuf, DriverError> {
    let write = |name: &str, text: &str| {
        let path = dir.join(name);
        fs::write(&path, text).map_err(|source| DriverError::Write { path, source })
    };
    let mut names = Vec::new();
    for (at, unit) in units.iter().enumerate() {
        names.push(format!("unit{at}.c"));
        write(&names[at], unit)?;
    }
    write(HEADER_NAME, &own_header(driver.target))?;
    let sources = sources(driver.target.os);
    for (name, text) in sources.iter().chain([&INTERNAL]) {
        write(name, text)?;
    }
    let mut clang = driver.clang(dir);
    clang.args(driver.compile_flags(dir));
    clang.args(["-g", "-gdwarf-4", "-fno-omit-frame-pointer"]);
    clang.args([
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=undefined",
    ]);
    clang.args(driver.link_flags());
    clang.args(&names);
    clang.args(sources.iter().map(|(name, _)| name));
    clang.args(["-o", "program"]);
    driver.run(clang)?;
    Ok(dir.join("program"))
}
