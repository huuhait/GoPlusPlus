use std::fs;
use std::path::Path;

use gopp_diag::Sink;
use gopp_span::SourceMap;
use gopp_syntax::constraint::BuildContext;
use lasso::ThreadedRodeo;
use tempfile::TempDir;

use super::{Roots, load};

fn write(root: &Path, file: &str, text: &str) {
    let path = root.join(file);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, text).unwrap();
}

fn linux() -> BuildContext {
    BuildContext {
        os: "linux".to_owned(),
        arch: "amd64".to_owned(),
        ..BuildContext::default()
    }
}

/// The packages of the program in `main.gopp` in the order of the graph,
/// each with its files, and what was reported.
fn graph(root: &Path, cx: &BuildContext) -> (Vec<String>, Vec<String>) {
    let roots = Roots {
        std: root.join("std"),
    };
    let (sources, names, sink) = (SourceMap::new(), ThreadedRodeo::new(), Sink::new(100));
    let main = [root.join("main.gopp")];
    let graph = load(("main", &main), &roots, cx, &sources, &names, &sink).unwrap();
    let name = |path: &Path| path.file_name().unwrap().to_string_lossy().into_owned();
    let packages = graph.packages.iter().map(|package| {
        let files = package.files.iter().map(|(_, source)| name(source.path()));
        let files: Vec<String> = files.collect();
        format!("{}: {}", package.path, files.join(" "))
    });
    let said = sink.finish().into_iter().map(|diagnostic| {
        let file = sources.file(diagnostic.span.file);
        format!("{}: {}", name(file.path()), diagnostic.message)
    });
    (packages.collect(), said.collect())
}

#[test]
fn a_package_comes_after_those_that_it_imports() {
    let dir = TempDir::new().unwrap();
    let root = dir.path();
    let main = "package main\nimport (\n\t\"text\"\n\t\"deep/leaf\"\n\t\"example.com/far\"\n\t\
                \"<vector>\"\n)\n";
    write(root, "main.gopp", main);
    write(
        root,
        "std/text/b.gopp",
        "package text\nimport \"deep/leaf\"\n",
    );
    write(root, "std/text/a.gopp", "package text\nimport \"base\"\n");
    write(
        root,
        "std/text/a_test.gopp",
        "package text\nimport \"nothing\"\n",
    );
    write(root, "std/text/.hidden.gopp", "package text\n");
    write(
        root,
        "std/deep/leaf/leaf.gopp",
        "package leaf\nimport \"base\"\n",
    );
    write(root, "std/base/base.gopp", "package base\n");
    write(root, "std/unused/u.gopp", "package unused\n");
    let (packages, said) = graph(root, &linux());
    assert_eq!(said, [] as [&str; 0]);
    let expected = [
        "base: base.gopp",
        "deep/leaf: leaf.gopp",
        "text: a.gopp b.gopp",
        "main: main.gopp",
    ];
    assert_eq!(packages, expected);
}

#[test]
fn the_files_of_a_package_are_those_of_the_system_that_is_built_for() {
    let dir = TempDir::new().unwrap();
    let root = dir.path();
    write(root, "main.gopp", "package main\nimport \"screen\"\n");
    write(root, "std/screen/screen.gopp", "package screen\n");
    write(root, "std/screen/draw_linux.gopp", "package screen\n");
    let windows = "package screen\nimport \"winapi\"\n";
    write(root, "std/screen/draw_windows.gopp", windows);
    write(
        root,
        "std/screen/arm.gopp",
        "@Build(arm64)\npackage screen\n",
    );
    write(root, "std/winapi/api.gopp", "package winapi\n");
    let (packages, said) = graph(root, &linux());
    assert_eq!(said, [] as [&str; 0]);
    let expected = ["screen: draw_linux.gopp screen.gopp", "main: main.gopp"];
    assert_eq!(packages, expected);
    let windows = BuildContext {
        os: "windows".to_owned(),
        ..linux()
    };
    let (packages, _) = graph(root, &windows);
    let expected = [
        "winapi: api.gopp",
        "screen: draw_windows.gopp screen.gopp",
        "main: main.gopp",
    ];
    assert_eq!(packages, expected);
}

#[test]
fn an_import_cycle_and_a_package_without_files_are_reported() {
    let dir = TempDir::new().unwrap();
    let root = dir.path();
    write(
        root,
        "main.gopp",
        "package main\nimport \"a\"\nimport \"only\"\n",
    );
    write(root, "std/a/a.gopp", "package a\nimport \"b\"\n");
    write(root, "std/b/b.gopp", "package b\nimport \"c\"\n");
    write(
        root,
        "std/c/c.gopp",
        "package c\nimport \"a\"\nimport \"c\"\n",
    );
    write(root, "std/only/only_windows.gopp", "package only\n");
    let (packages, said) = graph(root, &linux());
    let expected = [
        "main.gopp: package only: no source files for this build",
        "c.gopp: import cycle not allowed: a imports b imports c imports a",
        "c.gopp: import cycle not allowed: c imports c",
    ];
    assert_eq!(said, expected);
    let expected = [
        "c: c.gopp",
        "b: b.gopp",
        "a: a.gopp",
        "only: ",
        "main: main.gopp",
    ];
    assert_eq!(packages, expected);
}

/// A chain of packages that all import one more is ordered in one walk.
#[test]
fn many_packages_are_ordered_in_one_walk() {
    let dir = TempDir::new().unwrap();
    let root = dir.path();
    let count = 300;
    let main = format!("package main\nimport \"p{}\"\n", count - 1);
    write(root, "main.gopp", &main);
    write(root, "std/shared/s.gopp", "package shared\n");
    write(root, "std/p0/p.gopp", "package p0\nimport \"shared\"\n");
    for at in 1..count {
        let text = format!("package p{at}\nimport \"p{}\"\nimport \"shared\"\n", at - 1);
        write(root, &format!("std/p{at}/p.gopp"), &text);
    }
    let (packages, said) = graph(root, &linux());
    assert_eq!(said, [] as [&str; 0]);
    assert_eq!(packages.len(), count + 2);
    assert_eq!(packages[0], "shared: s.gopp");
    assert_eq!(packages[count], format!("p{}: p.gopp", count - 1));
}
