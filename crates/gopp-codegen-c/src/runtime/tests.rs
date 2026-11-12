use std::path::{Path, PathBuf};
use std::process::Command;

use gopp_target::{TargetLayout, TargetSpec};
use rayon::prelude::*;

use super::{FUNCS, Form, build, header};
use crate::driver::{ClangDriver, DriverError, OptLevel, Output};
use crate::toolchain::HostToolchain;

fn driver<'t>(toolchain: &'t HostToolchain, target: &str, opt: OptLevel) -> ClangDriver<'t> {
    ClangDriver {
        toolchain,
        target: TargetSpec::named(target).unwrap(),
        opt,
        strict: true,
    }
}

#[test]
fn the_header_says_what_both_sides_share() {
    insta::assert_snapshot!(header(TargetLayout::new(64)));
}

/// A unit that is the header and a table of every function of it.
fn names_all(layout: TargetLayout) -> String {
    let names: Vec<String> = (FUNCS.iter())
        .map(|func| format!("(void *){}", func.name))
        .collect();
    format!(
        "{}\nvoid *const all[] = {{{}}};\n\nint main(void) {{ return all[0] == 0; }}\n",
        header(layout),
        names.join(", ")
    )
}

#[test]
#[cfg_attr(
    windows,
    ignore = "needs Clang 23, which the Windows test machine does not have yet"
)]
fn the_header_compiles_for_both_targets_and_its_checks_bite() {
    let toolchain = HostToolchain::discover().unwrap();
    for target in ["linux/amd64", "windows/amd64"] {
        let driver = driver(&toolchain, target, OptLevel::O0);
        let compiled = |layout: TargetLayout| {
            let dir = tempfile::tempdir().unwrap();
            let object = dir.path().join("unit.o");
            driver.build(&names_all(layout), dir.path(), &object, Output::Object, &[])
        };
        compiled(driver.target.layout()).unwrap_or_else(|error| panic!("{target}: {error}"));
        // The header of a target with pointers of another width.
        let refused = compiled(TargetLayout::new(32)).unwrap_err().to_string();
        assert!(refused.contains("the width of a pointer"), "{refused}");
        assert!(refused.contains("the layout of gopp_iface"), "{refused}");
    }
}

fn magic(path: &Path) -> Vec<u8> {
    let bytes = std::fs::read(path).unwrap();
    bytes[..4].to_vec()
}

#[test]
#[cfg_attr(
    windows,
    ignore = "needs Clang 23, which the Windows test machine does not have yet"
)]
fn the_objects_are_made_for_both_targets_without_a_c_library() {
    let toolchain = HostToolchain::discover().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let made = |target: &str| {
        let driver = driver(&toolchain, target, OptLevel::O2);
        build(&driver, Form::Objects, cache.path()).unwrap()
    };
    let (elf, coff) = (made("linux/amd64"), made("windows/amd64"));
    assert_eq!((elf.len(), coff.len()), (2, 2));
    for object in &elf {
        assert_eq!(magic(object), b"\x7fELF");
    }
    for object in &coff {
        assert_eq!(magic(object)[..2], [0x64, 0x86]);
    }
    // The shared form is for the test runner, which is written for
    // Linux.
    let windows = driver(&toolchain, "windows/amd64", OptLevel::O2);
    let refused = build(&windows, Form::Shared, cache.path()).unwrap_err();
    assert!(
        matches!(refused, DriverError::NoSharedRuntime { .. }),
        "{refused}"
    );
}

/// The directories of the builds that a cache holds.
fn kept_in(cache: &Path) -> Vec<PathBuf> {
    let entries = std::fs::read_dir(cache.join("runtime")).unwrap();
    let mut kept: Vec<PathBuf> = entries.map(|entry| entry.unwrap().path()).collect();
    kept.sort();
    kept
}

#[test]
#[cfg_attr(
    windows,
    ignore = "needs Clang 23, which the Windows test machine does not have yet"
)]
fn the_runtime_is_built_once_for_a_target_and_its_flags() {
    let toolchain = HostToolchain::discover().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let plain = driver(&toolchain, "linux/amd64", OptLevel::O0);
    // Many builds at once of what is not there yet.
    let all: Vec<Vec<PathBuf>> = (0..16)
        .into_par_iter()
        .map(|_| build(&plain, Form::Objects, cache.path()).unwrap())
        .collect();
    assert!(all.iter().all(|objects| *objects == all[0]));
    let kept = kept_in(cache.path());
    assert_eq!(kept.len(), 1, "{kept:?}");
    // What is kept is not made again.
    let written = |path: &Path| std::fs::metadata(path).unwrap().modified().unwrap();
    let first = written(&all[0][0]);
    assert_eq!(build(&plain, Form::Objects, cache.path()).unwrap(), all[0]);
    assert_eq!(written(&all[0][0]), first);
    // The driver of a program, which lets warnings be, has the same.
    let loose = ClangDriver {
        strict: false,
        ..driver(&toolchain, "linux/amd64", OptLevel::O0)
    };
    assert_eq!(build(&loose, Form::Objects, cache.path()).unwrap(), all[0]);
    // Other flags, another target and another form are other builds.
    let optimized = driver(&toolchain, "linux/amd64", OptLevel::O2);
    let windows = driver(&toolchain, "windows/amd64", OptLevel::O0);
    assert_ne!(
        build(&optimized, Form::Objects, cache.path()).unwrap(),
        all[0]
    );
    assert_ne!(
        build(&windows, Form::Objects, cache.path()).unwrap(),
        all[0]
    );
    assert_eq!(kept_in(cache.path()).len(), 3);
    // A directory that has the name and is not the build makes way.
    std::fs::write(kept[0].join("key"), "another").unwrap();
    assert_eq!(build(&plain, Form::Objects, cache.path()).unwrap(), all[0]);
    assert_eq!(kept_in(cache.path()).len(), 3);
    assert_ne!(std::fs::read(kept[0].join("key")).unwrap(), b"another");
}

#[test]
#[cfg_attr(
    not(all(target_os = "linux", target_arch = "x86_64")),
    ignore = "links and runs programs for linux/amd64"
)]
fn the_runtime_defines_every_function_in_both_forms() {
    let toolchain = HostToolchain::discover().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let driver = driver(&toolchain, "linux/amd64", OptLevel::O0);
    let layout = driver.target.layout();
    // A program that names them all, and one that fails a check.
    let fails = format!(
        "{}\nint main(int argc, char **argv) {{\n  _gopp_rt_init(argc, argv);\n  _gopp_panic_nil();\n}}\n",
        header(layout)
    );
    for form in [Form::Objects, Form::Shared] {
        let runtime = build(&driver, form, cache.path()).unwrap();
        let folder = runtime[0].parent().unwrap();
        for (unit, status) in [(names_all(layout), 0), (fails.clone(), 2)] {
            let dir = tempfile::tempdir().unwrap();
            let program = dir.path().join("program");
            driver
                .build(&unit, dir.path(), &program, Output::Executable, &runtime)
                .unwrap_or_else(|error| panic!("{form:?}: {error}"));
            let mut command = Command::new(&program);
            command.env("LD_LIBRARY_PATH", folder);
            assert_eq!(command.status().unwrap().code(), Some(status), "{form:?}");
        }
    }
}
