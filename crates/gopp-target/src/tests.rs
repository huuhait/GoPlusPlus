use target_lexicon::{BinaryFormat, Endianness, PointerWidth, Triple};

use super::*;

#[test]
fn the_table_lists_the_eight_targets_of_the_design() {
    let expected = "\
TARGET         TRIPLE                       POINTER  LONG     CALLS      LINKER                  OBJECT  VERSION 1
linux/amd64    x86_64-unknown-linux-gnu     64 bits  64 bits  swiftcall  ld.lld -m elf_x86_64    ELF     supported
linux/arm64    aarch64-unknown-linux-gnu    64 bits  64 bits  swiftcall  ld.lld -m aarch64linux  ELF     not yet
linux/riscv64  riscv64gc-unknown-linux-gnu  64 bits  64 bits  C          ld.lld -m elf64lriscv   ELF     not yet
darwin/arm64   aarch64-apple-darwin         64 bits  64 bits  swiftcall  ld64.lld                Mach-O  not yet
darwin/amd64   x86_64-apple-darwin          64 bits  64 bits  swiftcall  ld64.lld                Mach-O  not yet
windows/amd64  x86_64-pc-windows-msvc       64 bits  32 bits  swiftcall  lld-link                COFF    supported
windows/arm64  aarch64-pc-windows-msvc      64 bits  32 bits  swiftcall  lld-link                COFF    not yet
wasip1/wasm32  wasm32-wasip1                32 bits  32 bits  C          wasm-ld                 Wasm    not yet
";
    assert_eq!(TargetSpec::table(), expected);
}

#[test]
fn every_row_says_what_its_triple_means() {
    for target in &TARGETS {
        let triple: Triple = target.triple.parse().unwrap();
        let width = match triple.pointer_width().unwrap() {
            PointerWidth::U16 => 16,
            PointerWidth::U32 => 32,
            PointerWidth::U64 => 64,
        };
        assert_eq!(target.pointer_bits, width, "{target}");
        assert_eq!(triple.endianness().unwrap(), Endianness::Little, "{target}");
        let format = match triple.binary_format {
            // The triple of WASI has no format of its own there.
            _ if target.arch == Arch::Wasm32 => ObjectFormat::Wasm,
            BinaryFormat::Elf => ObjectFormat::Elf,
            BinaryFormat::Macho => ObjectFormat::MachO,
            BinaryFormat::Coff => ObjectFormat::Coff,
            other => panic!("{target}: {other:?}"),
        };
        assert_eq!(target.object, format, "{target}");
        // C's `long` is as wide as a pointer, but not on Windows.
        let long = if target.os == Os::Windows { 32 } else { width };
        assert_eq!(target.c_long_bits, long, "{target}");
        assert_eq!(
            target.loader.is_some(),
            matches!(target.os, Os::Linux | Os::Darwin)
        );
        assert_eq!(TargetSpec::named(&target.to_string()), Ok(target));
    }
}

#[test]
fn a_target_is_named_by_the_flag_or_by_the_environment() {
    let host = TargetSpec::host().unwrap();
    assert_eq!(TargetSpec::selected(None, None, None), Ok(host));
    let named = |name: &str| TargetSpec::named(name).unwrap();
    let windows = named("windows/amd64");
    assert_eq!(
        TargetSpec::selected(Some("windows/amd64"), None, None),
        Ok(windows)
    );
    // The flag is said last and wins.
    let both = TargetSpec::selected(Some("windows/amd64"), Some("linux"), Some("arm64"));
    assert_eq!(both, Ok(windows));
    let arm = TargetSpec::selected(None, Some("linux"), Some("arm64"));
    assert_eq!(arm, Ok(named("linux/arm64")));
    // One of the two stands in for that part of the host.
    let os_only = TargetSpec::selected(None, Some(host.os.name()), None);
    assert_eq!(os_only, Ok(host));
    let arch_only = TargetSpec::selected(None, None, Some(host.arch.name()));
    assert_eq!(arch_only, Ok(host));

    let unknown = TargetSpec::selected(Some("plan9/amd64"), None, None);
    assert_eq!(unknown, Err(TargetError::Unknown("plan9/amd64".to_owned())));
    let message = unknown.unwrap_err().to_string();
    assert_eq!(
        message,
        "unknown target plan9/amd64: `gopp targets` lists the targets"
    );
    let mixed = TargetSpec::selected(None, Some("wasip1"), Some("amd64"));
    assert_eq!(mixed, Err(TargetError::Unknown("wasip1/amd64".to_owned())));
}

#[test]
fn version_one_builds_for_two_targets() {
    let supported: Vec<String> = TARGETS
        .iter()
        .filter(|target| target.in_this_version().is_ok())
        .map(ToString::to_string)
        .collect();
    assert_eq!(supported, ["linux/amd64", "windows/amd64"]);
    let refused = TargetSpec::named("darwin/arm64").unwrap().in_this_version();
    assert_eq!(
        refused.unwrap_err().to_string(),
        "target darwin/arm64 is not supported in this version: version 1 builds for linux/amd64 \
         and windows/amd64"
    );
}

/// `size/align` of the layout.
fn shown(layout: Layout) -> String {
    format!("{}/{}", layout.size, layout.align)
}

#[test]
fn a_header_has_its_fields_where_c_puts_them() {
    let (wide, narrow) = (TargetLayout::new(64), TargetLayout::new(32));
    let placed = |layout: TargetLayout, header: &Header| {
        let (whole, offsets) = layout.header(header);
        (shown(whole), offsets)
    };
    assert_eq!(placed(wide, &STRING), ("24/8".to_owned(), vec![0, 8, 16]));
    assert_eq!(
        placed(wide, &SLICE),
        ("32/8".to_owned(), vec![0, 8, 16, 24])
    );
    assert_eq!(placed(wide, &IFACE), ("16/8".to_owned(), vec![0, 8]));
    // A count is 64 bits wide where a pointer is not.
    assert_eq!(placed(narrow, &STRING), ("24/8".to_owned(), vec![0, 8, 16]));
    assert_eq!(
        placed(narrow, &SLICE),
        ("32/8".to_owned(), vec![0, 8, 16, 24])
    );
    assert_eq!(placed(narrow, &IFACE), ("8/4".to_owned(), vec![0, 4]));
    // The names are those of C, each once.
    let names: Vec<&str> = HEADERS.iter().map(|header| header.name).collect();
    assert_eq!(names, ["gopp_string", "gopp_slice", "gopp_iface"]);
}

#[test]
fn values_are_as_large_as_the_target_says() {
    let table = |name: &str| {
        let layout = TargetSpec::named(name).unwrap().layout();
        let int = layout.scalar(layout.pointer_bits());
        [
            ("bool", layout.bool()),
            ("int8", layout.scalar(8)),
            ("int16", layout.scalar(16)),
            ("int32", layout.scalar(32)),
            ("int64", layout.scalar(64)),
            ("float64", layout.scalar(64)),
            ("int", int),
            ("pointer", layout.pointer()),
            ("string", layout.string()),
            ("slice", layout.slice()),
            ("interface", layout.interface()),
            ("func", layout.func_value()),
            ("Option[int32]", layout.option(layout.scalar(32)).unwrap()),
            ("Option[int8]", layout.option(layout.scalar(8)).unwrap()),
            ("Option[string]", layout.option(layout.string()).unwrap()),
            ("[3]int16", layout.scalar(16).array(3).unwrap()),
            ("[0]int64", layout.scalar(64).array(0).unwrap()),
            ("struct{}", layout.aggregate(&[]).unwrap().0),
        ]
        .map(|(what, layout)| format!("{what} {}", shown(layout)))
        .join(", ")
    };
    // Every 64-bit target has one table.
    let wide = "bool 1/1, int8 1/1, int16 2/2, int32 4/4, int64 8/8, float64 8/8, int 8/8, \
                pointer 8/8, string 24/8, slice 32/8, interface 16/8, func 16/8, \
                Option[int32] 8/4, Option[int8] 2/1, Option[string] 32/8, [3]int16 6/2, \
                [0]int64 0/8, struct{} 0/1";
    for target in TARGETS.iter().filter(|target| target.pointer_bits == 64) {
        assert_eq!(table(&target.to_string()), wide, "{target}");
    }
    // On 32 bits the lengths of a string and of a slice stay 64 bits.
    let narrow = "bool 1/1, int8 1/1, int16 2/2, int32 4/4, int64 8/8, float64 8/8, int 4/4, \
                  pointer 4/4, string 24/8, slice 32/8, interface 8/4, func 8/4, \
                  Option[int32] 8/4, Option[int8] 2/1, Option[string] 32/8, [3]int16 6/2, \
                  [0]int64 0/8, struct{} 0/1";
    assert_eq!(table("wasip1/wasm32"), narrow);
}

#[test]
fn fields_are_laid_out_as_c_lays_out_a_struct() {
    let layout = TargetSpec::named("linux/amd64").unwrap().layout();
    let (byte, int32, int64) = (layout.scalar(8), layout.scalar(32), layout.scalar(64));
    // `struct{ a int8; b int64; c int8; d int32 }`.
    let (whole, offsets) = layout.aggregate(&[byte, int64, byte, int32]).unwrap();
    assert_eq!(
        (shown(whole), offsets),
        ("24/8".to_owned(), vec![0, 8, 16, 20])
    );
    // The same fields from the largest to the smallest need no padding
    // but at the end.
    let (whole, offsets) = layout.aggregate(&[int64, int32, byte, byte]).unwrap();
    assert_eq!(
        (shown(whole), offsets),
        ("16/8".to_owned(), vec![0, 8, 12, 13])
    );
    // A field without a size has a place.
    let (whole, offsets) = layout.aggregate(&[byte, Layout::UNIT, int32]).unwrap();
    assert_eq!((shown(whole), offsets), ("8/4".to_owned(), vec![0, 1, 4]));
    // What is too large to have a size has none.
    assert_eq!(int64.array(u64::MAX), None);
    let huge = Layout {
        size: u64::MAX - 7,
        align: 8,
    };
    assert_eq!(layout.aggregate(&[huge, int64]), None);
    assert_eq!(layout.option(huge), None);
}
