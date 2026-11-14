//! What a program frees. Every program of `tests/run` that ends well is
//! built with the address, the leak and the undefined-behaviour
//! sanitizers of Clang, the runtime too, and must run without a report:
//! nothing is read after it is freed, nothing is freed twice, and what
//! is made is freed.

use std::path::Path;

use super::*;

#[test]
#[ignore = "long: builds every run test with the sanitizers of Clang"]
fn no_program_of_tests_run_leaks_or_reads_what_is_freed() {
    let toolchain = HostToolchain::discover().unwrap();
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/run");
    let mut programs: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| gopp_span::is_source_path(path))
        .collect();
    programs.sort();
    let failed: Vec<String> = programs
        .par_iter()
        .filter_map(|path| {
            let text = std::fs::read_to_string(path).unwrap();
            // A program that ends in a panic frees nothing on its way;
            // one that imports a package is built by the tool, whose
            // tests have it.
            if text.starts_with("// exit:") || text.contains("\nimport ") {
                return None;
            }
            let name = path.file_name().unwrap().to_string_lossy().into_owned();
            let unit = emit_c23(&module(&name, &text), &options(Entry::Main)).unwrap();
            // The C that the program calls is a file beside it.
            let mut units = vec![unit];
            units.extend(std::fs::read_to_string(path.with_extension("c")));
            let dir = tempfile::tempdir().unwrap();
            let driver = ClangDriver {
                strict: false,
                ..strict(&toolchain, "linux/amd64", OptLevel::O1)
            };
            let program = match crate::runtime::build_sanitized(&driver, &units, dir.path()) {
                Ok(program) => program,
                Err(error) => return Some(format!("{name}: {error}")),
            };
            let ran = std::process::Command::new(program)
                .env("ASAN_OPTIONS", "detect_leaks=1")
                .output()
                .unwrap();
            if ran.status.success() {
                return None;
            }
            let said = String::from_utf8_lossy(&ran.stderr);
            let report: Vec<&str> = said
                .lines()
                .filter(|line| line.contains("ERROR") || line.contains("SUMMARY"))
                .collect();
            Some(format!("{name}: {}", report.join("; ")))
        })
        .collect();
    assert_eq!(failed, [] as [&str; 0]);
}
