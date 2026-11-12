//! Target specifications and type layout: the one table of what differs
//! between the machines that Go++ builds for. Nothing else in the
//! compiler writes a target triple or guesses the width of a pointer.

mod layout;

use std::fmt;

pub use layout::{HEADERS, Header, IFACE, Layout, Part, SLICE, STRING, TargetLayout};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Os {
    Linux,
    Darwin,
    Windows,
    Wasip1,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Arch {
    Amd64,
    Arm64,
    Riscv64,
    Wasm32,
}

/// The linker of LLD that links for a target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Lld {
    /// `ld.lld`, with the emulation that `-m` names.
    Elf(&'static str),
    /// `ld64.lld`.
    MachO,
    /// `lld-link`.
    Coff,
    /// `wasm-ld`.
    Wasm,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObjectFormat {
    Elf,
    MachO,
    Coff,
    Wasm,
}

/// What the compiler knows of one target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TargetSpec {
    pub os: Os,
    pub arch: Arch,
    /// What Clang takes as `--target`.
    pub triple: &'static str,
    pub pointer_bits: u8,
    /// The width of C's `long`: that of a pointer, but 32 on Windows.
    pub c_long_bits: u8,
    /// Whether Clang has the `swiftcall` convention here; without it
    /// calls between Go++ functions use the C convention.
    pub swiftcall: bool,
    pub lld: Lld,
    pub object: ObjectFormat,
    /// Where the dynamic loader is on a system that follows the usual
    /// layout, for a target that has one.
    pub loader: Option<&'static str>,
    /// What `-march` is given unless a profile names a CPU: never the
    /// CPU of the machine that builds.
    pub cpu: Option<&'static str>,
    /// Whether version 1 supports the target (ADR-0037).
    pub supported: bool,
}

/// Why no target was selected.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum TargetError {
    #[error("unknown target {0}: `gopp targets` lists the targets")]
    Unknown(String),
    #[error("target {0} is not supported in this version: version 1 builds for {SUPPORTED}")]
    NotInThisVersion(String),
    #[error("gopp does not build for this machine: name a target with -target=os/arch")]
    NoHost,
}

/// The targets that version 1 supports, as a message names them.
const SUPPORTED: &str = "linux/amd64 and windows/amd64";

const fn spec(os: Os, arch: Arch, triple: &'static str, lld: Lld) -> TargetSpec {
    let pointer_bits = match arch {
        Arch::Wasm32 => 32,
        _ => 64,
    };
    let c_long_bits = match os {
        Os::Windows => 32,
        _ => pointer_bits,
    };
    let object = match lld {
        Lld::Elf(_) => ObjectFormat::Elf,
        Lld::MachO => ObjectFormat::MachO,
        Lld::Coff => ObjectFormat::Coff,
        Lld::Wasm => ObjectFormat::Wasm,
    };
    let loader = match (os, arch) {
        (Os::Linux, Arch::Amd64) => Some("/lib64/ld-linux-x86-64.so.2"),
        (Os::Linux, Arch::Arm64) => Some("/lib/ld-linux-aarch64.so.1"),
        (Os::Linux, Arch::Riscv64) => Some("/lib/ld-linux-riscv64-lp64d.so.1"),
        (Os::Darwin, _) => Some("/usr/lib/dyld"),
        _ => None,
    };
    let cpu = match arch {
        Arch::Amd64 => Some("x86-64"),
        Arch::Arm64 => Some("armv8-a"),
        Arch::Riscv64 => Some("rv64gc"),
        Arch::Wasm32 => None,
    };
    TargetSpec {
        os,
        arch,
        triple,
        pointer_bits,
        c_long_bits,
        swiftcall: matches!(arch, Arch::Amd64 | Arch::Arm64),
        lld,
        object,
        loader,
        cpu,
        supported: matches!((os, arch), (Os::Linux | Os::Windows, Arch::Amd64)),
    }
}

/// Every target, in the order of the design.
pub const TARGETS: [TargetSpec; 8] = [
    spec(
        Os::Linux,
        Arch::Amd64,
        "x86_64-unknown-linux-gnu",
        Lld::Elf("elf_x86_64"),
    ),
    spec(
        Os::Linux,
        Arch::Arm64,
        "aarch64-unknown-linux-gnu",
        Lld::Elf("aarch64linux"),
    ),
    spec(
        Os::Linux,
        Arch::Riscv64,
        "riscv64gc-unknown-linux-gnu",
        Lld::Elf("elf64lriscv"),
    ),
    spec(Os::Darwin, Arch::Arm64, "aarch64-apple-darwin", Lld::MachO),
    spec(Os::Darwin, Arch::Amd64, "x86_64-apple-darwin", Lld::MachO),
    spec(
        Os::Windows,
        Arch::Amd64,
        "x86_64-pc-windows-msvc",
        Lld::Coff,
    ),
    spec(
        Os::Windows,
        Arch::Arm64,
        "aarch64-pc-windows-msvc",
        Lld::Coff,
    ),
    spec(Os::Wasip1, Arch::Wasm32, "wasm32-wasip1", Lld::Wasm),
];

impl Os {
    pub fn name(self) -> &'static str {
        match self {
            Os::Linux => "linux",
            Os::Darwin => "darwin",
            Os::Windows => "windows",
            Os::Wasip1 => "wasip1",
        }
    }
}

impl Arch {
    pub fn name(self) -> &'static str {
        match self {
            Arch::Amd64 => "amd64",
            Arch::Arm64 => "arm64",
            Arch::Riscv64 => "riscv64",
            Arch::Wasm32 => "wasm32",
        }
    }
}

impl ObjectFormat {
    pub fn name(self) -> &'static str {
        match self {
            ObjectFormat::Elf => "ELF",
            ObjectFormat::MachO => "Mach-O",
            ObjectFormat::Coff => "COFF",
            ObjectFormat::Wasm => "Wasm",
        }
    }
}

impl Lld {
    /// The name of the linker program.
    pub fn program(self) -> &'static str {
        match self {
            Lld::Elf(_) => "ld.lld",
            Lld::MachO => "ld64.lld",
            Lld::Coff => "lld-link",
            Lld::Wasm => "wasm-ld",
        }
    }
}

impl fmt::Display for TargetSpec {
    /// `os/arch`, as `-target` takes it.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}/{}", self.os.name(), self.arch.name())
    }
}

impl TargetSpec {
    /// The target that `os/arch` names.
    pub fn named(name: &str) -> Result<&'static TargetSpec, TargetError> {
        let found = TARGETS.iter().find(|target| target.to_string() == name);
        found.ok_or_else(|| TargetError::Unknown(name.to_owned()))
    }

    /// The target of the machine that the compiler runs on.
    pub fn host() -> Result<&'static TargetSpec, TargetError> {
        let host = target_lexicon::HOST;
        let found = TARGETS.iter().find(|target| {
            let Ok(triple) = target.triple.parse::<target_lexicon::Triple>() else {
                return false;
            };
            triple.architecture == host.architecture
                && triple.operating_system == host.operating_system
        });
        found.ok_or(TargetError::NoHost)
    }

    /// The target that a build is for: what `-target=os/arch` names;
    /// without it, what `GOPPOS` and `GOPPARCH` name, each standing in
    /// for that part of the host; without them, the host.
    pub fn selected(
        flag: Option<&str>,
        goppos: Option<&str>,
        gopparch: Option<&str>,
    ) -> Result<&'static TargetSpec, TargetError> {
        if let Some(name) = flag {
            return Self::named(name);
        }
        if goppos.is_none() && gopparch.is_none() {
            return Self::host();
        }
        let host = Self::host().ok();
        let os = goppos.or(host.map(|host| host.os.name()));
        let arch = gopparch.or(host.map(|host| host.arch.name()));
        match (os, arch) {
            (Some(os), Some(arch)) => Self::named(&format!("{os}/{arch}")),
            _ => Err(TargetError::NoHost),
        }
    }

    /// The target, if version 1 builds for it.
    pub fn in_this_version(&'static self) -> Result<&'static TargetSpec, TargetError> {
        match self.supported {
            true => Ok(self),
            false => Err(TargetError::NotInThisVersion(self.to_string())),
        }
    }

    pub fn layout(&self) -> TargetLayout {
        TargetLayout::new(self.pointer_bits)
    }

    /// The table of all targets, as `gopp targets` prints it.
    pub fn table() -> String {
        let mut rows = vec![[
            "TARGET".to_owned(),
            "TRIPLE".to_owned(),
            "POINTER".to_owned(),
            "LONG".to_owned(),
            "CALLS".to_owned(),
            "LINKER".to_owned(),
            "OBJECT".to_owned(),
            "VERSION 1".to_owned(),
        ]];
        for target in &TARGETS {
            let linker = match target.lld {
                Lld::Elf(emulation) => format!("ld.lld -m {emulation}"),
                other => other.program().to_owned(),
            };
            rows.push([
                target.to_string(),
                target.triple.to_owned(),
                format!("{} bits", target.pointer_bits),
                format!("{} bits", target.c_long_bits),
                if target.swiftcall { "swiftcall" } else { "C" }.to_owned(),
                linker,
                target.object.name().to_owned(),
                if target.supported {
                    "supported"
                } else {
                    "not yet"
                }
                .to_owned(),
            ]);
        }
        let mut widths = [0; 8];
        for row in &rows {
            for (width, cell) in widths.iter_mut().zip(row) {
                *width = (*width).max(cell.len());
            }
        }
        let mut out = String::new();
        for row in &rows {
            let cells = row.iter().zip(widths);
            let line: Vec<String> = cells
                .map(|(cell, width)| format!("{cell:<width$}"))
                .collect();
            out += line.join("  ").trim_end();
            out.push('\n');
        }
        out
    }
}

#[cfg(test)]
mod tests;
