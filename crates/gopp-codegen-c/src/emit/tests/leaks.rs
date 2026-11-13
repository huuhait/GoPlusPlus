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
    let target = TargetSpec::named("linux/amd64").unwrap();
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/run");
    let mut programs: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| {
            let kind = path.extension().and_then(|kind| kind.to_str());
            matches!(kind, Some("go" | "gopp"))
        })
        .collect();
    programs.sort();
    let failed: Vec<String> = programs
        .par_iter()
        .filter_map(|path| {
            let text = std::fs::read_to_string(path).unwrap();
            // A program that ends in a panic frees nothing on its way.
            if text.starts_with("// exit:") {
                return None;
            }
            let name = path.file_name().unwrap().to_string_lossy().into_owned();
            let unit = emit_c23(&module(&name, &text), &options(Entry::Main)).unwrap();
            let dir = tempfile::tempdir().unwrap();
            let write = |name: &str, text: &str| std::fs::write(dir.path().join(name), text);
            write("unit.c", &unit).unwrap();
            write(
                crate::runtime::HEADER_NAME,
                &crate::runtime::header(target.layout()),
            )
            .unwrap();
            let sources = crate::runtime::sources(target.os);
            for (name, text) in sources.iter().chain([&crate::runtime::INTERNAL]) {
                write(name, text).unwrap();
            }
            let driver = ClangDriver {
                strict: false,
                ..strict(&toolchain, "linux/amd64", OptLevel::O1)
            };
            let mut clang = driver.clang(dir.path());
            clang.args(driver.compile_flags(dir.path()));
            clang.args(["-g", "-gdwarf-4", "-fno-omit-frame-pointer"]);
            clang.args([
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=undefined",
            ]);
            clang.args(driver.link_flags());
            clang.arg("unit.c");
            clang.args(sources.iter().map(|(name, _)| name));
            clang.args(["-o", "program"]);
            if let Err(error) = driver.run(clang) {
                return Some(format!("{name}: {error}"));
            }
            let ran = std::process::Command::new(dir.path().join("program"))
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
