//! Clang as the back end: it compiles a unit of C and links an
//! executable (targets-and-linking.md). The flags that decide what a
//! program means are said here, in one place.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

use gopp_target::{ObjectFormat, TargetSpec};

use crate::toolchain::HostToolchain;

/// What Clang makes of a unit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Output {
    Object,
    Assembly,
    Executable,
    /// A shared object that a process loads while it runs.
    Module,
}

/// How much Clang optimizes. A program means the same at every level.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum OptLevel {
    #[default]
    O0,
    O1,
    O2,
    O3,
    Os,
    Oz,
}

impl OptLevel {
    fn flag(self) -> &'static str {
        match self {
            Self::O0 => "-O0",
            Self::O1 => "-O1",
            Self::O2 => "-O2",
            Self::O3 => "-O3",
            Self::Os => "-Os",
            Self::Oz => "-Oz",
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum DriverError {
    #[error("cannot write {}: {source}", path.display())]
    Write { path: PathBuf, source: io::Error },
    #[error("cannot run {}: {source}", path.display())]
    Run { path: PathBuf, source: io::Error },
    #[error("clang failed:\n{stderr}")]
    Failed { stderr: String },
    #[error(
        "cannot link for {target}: {name} is not found; the C library of the target is needed \
         (`--sysroot` and `-crt-dir` say where it is)"
    )]
    MissingLinkInput { target: &'static str, name: String },
    #[error(
        "cannot link for {target} on this machine without the C library of the target: \
         `--sysroot` or `-crt-dir` says where it is; `-emit=obj` makes the object file"
    )]
    NoSysroot { target: String },
    #[error(
        "cannot link for {target} on this machine: the link needs the libraries of Windows, \
         which are found on Windows only; `-emit=obj` makes the object file"
    )]
    NoWindowsLibraries { target: String },
    #[error("the runtime has no shared form for {target} yet")]
    NoSharedRuntime { target: &'static str },
}

/// The flags without which a program does not mean what Go++ says: the
/// language, wrapping integers, no aliasing by type, no fused
/// floating-point operations, allocations that are not optimized away,
/// and tables for unwinding.
pub const SEMANTIC_FLAGS: [&str; 10] = [
    "-std=gnu23",
    "-fwrapv",
    "-fno-strict-aliasing",
    "-ffp-contract=off",
    "-fno-builtin-malloc",
    "-fno-builtin-calloc",
    "-fno-builtin-realloc",
    "-fno-builtin-free",
    "-fexceptions",
    "-fasynchronous-unwind-tables",
];

/// The flags that make a linked ELF file a function of its inputs.
const ELF_LINK_FLAGS: [&str; 4] = [
    "-Wl,--eh-frame-hdr",
    "-Wl,--hash-style=gnu",
    "-Wl,--build-id=none",
    "-Wl,-z,relro",
];

/// The files that a link for an ELF target needs of the C library.
const ELF_LINK_INPUTS: [&str; 4] = ["crt1.o", "crti.o", "crtn.o", "libc.so"];

/// Where a link for another system finds the C library of the target.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LinkInputs<'a> {
    /// The root of the files of the target (`--sysroot`).
    pub sysroot: Option<&'a Path>,
    /// The directory of the startup objects and of the C library
    /// (`-crt-dir`).
    pub crt_dir: Option<&'a Path>,
    /// The dynamic loader that the program names, in the place of the
    /// one of the table of the targets (`-dynamic-linker`).
    pub loader: Option<&'a str>,
}

#[derive(Clone, Copy)]
pub struct ClangDriver<'a> {
    pub toolchain: &'a HostToolchain,
    pub target: &'static TargetSpec,
    pub opt: OptLevel,
    /// Every warning of Clang about the unit is an error: for the tests
    /// of the emitter. Without it, the warnings are off.
    pub strict: bool,
    /// Clang writes what a debugger reads (debug-info.md).
    pub debug: bool,
    /// Clang writes where its time went, a file for each unit in the
    /// directory `TIME_TRACE_DIR` beside the unit.
    pub time_trace: bool,
    pub link: LinkInputs<'a>,
}

/// Where Clang writes its time traces, in the directory of a unit.
pub const TIME_TRACE_DIR: &str = "time-trace";

impl<'a> ClangDriver<'a> {
    /// A driver for the target that does not optimize, with the
    /// warnings off and nothing for a debugger.
    pub fn new(toolchain: &'a HostToolchain, target: &'static TargetSpec) -> Self {
        Self {
            toolchain,
            target,
            opt: OptLevel::O0,
            strict: false,
            debug: false,
            time_trace: false,
            link: LinkInputs::default(),
        }
    }
}

impl ClangDriver<'_> {
    /// The flags for compiling a unit that lies in `dir`.
    pub fn compile_flags(&self, dir: &Path) -> Vec<String> {
        let mut flags: Vec<String> = SEMANTIC_FLAGS.iter().map(|&flag| flag.to_owned()).collect();
        flags.push(format!("--target={}", self.target.triple));
        if let Some(cpu) = self.target.cpu {
            flags.push(format!("-march={cpu}"));
        }
        flags.push(self.opt.flag().to_owned());
        // DWARF, where the debuggers of the target read it. A column
        // would be one of the unit, which says lines of the source only.
        if self.debug && self.target.object == ObjectFormat::Elf {
            let dwarf = ["-gdwarf-5", "-gno-column-info", "-fno-omit-frame-pointer"];
            flags.extend(dwarf.map(str::to_owned));
            // A debugger shows the name of a function, which is that
            // of the source, and not its symbol.
            flags.extend(["-mllvm", "-dwarf-linkage-names=Abstract"].map(str::to_owned));
        }
        // A unit for a debugger declares each variable in a block of C
        // for its scope. The block is no lifetime: a variable keeps its
        // value while the function runs, as one that is declared at
        // the top of the function does.
        if self.debug {
            flags.extend(["-Xclang".to_owned(), "-disable-lifetime-markers".to_owned()]);
        }
        flags.push(format!("-ffile-prefix-map={}=.", dir.display()));
        // Clang writes the time into a COFF object unless it is told
        // that no incremental linker reads it.
        if self.target.object == ObjectFormat::Coff {
            flags.push("-mno-incremental-linker-compatible".to_owned());
        }
        if self.time_trace {
            flags.push(format!("-ftime-trace={TIME_TRACE_DIR}/"));
        }
        match self.strict {
            true => flags.extend(["-Wall".to_owned(), "-Werror".to_owned()]),
            false => flags.push("-w".to_owned()),
        }
        // A wrapped Clang passes flags of its own that another target
        // does not use; they say nothing about the unit.
        flags.push("-Wno-unused-command-line-argument".to_owned());
        flags
    }

    pub(crate) fn link_flags(&self) -> Vec<String> {
        let mut flags = vec!["-fuse-ld=lld".to_owned()];
        if self.target.object == ObjectFormat::Elf {
            flags.extend(ELF_LINK_FLAGS.map(str::to_owned));
        }
        flags.extend(self.found_flags());
        if let Some(loader) = self.link.loader {
            flags.push(format!("-Wl,--dynamic-linker={loader}"));
        }
        flags
    }

    /// The flags that say where the C library of the target is. Clang
    /// runs in the directory of the unit, so each path is a whole one.
    fn found_flags(&self) -> Vec<String> {
        let whole = |path: &Path| {
            let whole = std::path::absolute(path).unwrap_or_else(|_| path.to_owned());
            whole.display().to_string()
        };
        let mut flags = Vec::new();
        if let Some(sysroot) = self.link.sysroot {
            flags.push(format!("--sysroot={}", whole(sysroot)));
        }
        if let Some(dir) = self.link.crt_dir {
            flags.extend([format!("-B{}", whole(dir)), format!("-L{}", whole(dir))]);
        }
        flags
    }

    pub(crate) fn clang(&self, dir: &Path) -> Command {
        let mut command = Command::new(&self.toolchain.clang.path);
        // The unit is named without its directory, so that no path of
        // this machine gets into what is made.
        command.current_dir(dir);
        // A Clang of Nix adds flags of its own from these, which would
        // change the code and refuse another target.
        command.env("NIX_HARDENING_ENABLE", "");
        command.env_remove("NIX_CFLAGS_COMPILE");
        command
    }

    pub(crate) fn run(&self, mut command: Command) -> Result<String, DriverError> {
        let output = command.output().map_err(|source| DriverError::Run {
            path: self.toolchain.clang.path.clone(),
            source,
        })?;
        match output.status.success() {
            true => Ok(String::from_utf8_lossy(&output.stdout).into_owned()),
            false => Err(DriverError::Failed {
                stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
            }),
        }
    }

    /// Whether every file that a link needs is there, before the linker
    /// is asked: it would say less.
    pub fn check_link_inputs(&self) -> Result<(), DriverError> {
        let host = TargetSpec::host().ok();
        let own = host.is_some_and(|host| host.triple == self.target.triple);
        let target = format!("{}/{}", self.target.os.name(), self.target.arch.name());
        if self.target.object != ObjectFormat::Elf {
            return match own {
                true => Ok(()),
                false => Err(DriverError::NoWindowsLibraries { target }),
            };
        }
        // The C library of another system is where the build says.
        let said = self.link.sysroot.is_some() || self.link.crt_dir.is_some();
        if !own && !said {
            return Err(DriverError::NoSysroot { target });
        }
        for name in ELF_LINK_INPUTS {
            let mut command = self.clang(Path::new("."));
            command.arg(format!("--target={}", self.target.triple));
            command.args(self.found_flags());
            command.arg(format!("-print-file-name={name}"));
            let found = self.run(command)?;
            // Clang answers with the name alone for what it does not
            // find.
            if !Path::new(found.trim()).is_absolute() {
                return Err(DriverError::MissingLinkInput {
                    target: self.target.triple,
                    name: name.to_owned(),
                });
            }
        }
        Ok(())
    }

    /// Compiles the unit of C `unit` to `output`; an executable and a
    /// module are linked with `link_with` too. `dir` is a directory of its own for
    /// the unit.
    pub fn build(
        &self,
        unit: &str,
        dir: &Path,
        output: &Path,
        kind: Output,
        link_with: &[PathBuf],
    ) -> Result<(), DriverError> {
        let source = dir.join("unit.c");
        fs::write(&source, unit).map_err(|source_error| DriverError::Write {
            path: source.clone(),
            source: source_error,
        })?;
        let output = std::path::absolute(output).map_err(|source| DriverError::Write {
            path: output.to_owned(),
            source,
        })?;
        if self.time_trace {
            let traces = dir.join(TIME_TRACE_DIR);
            fs::create_dir_all(&traces).map_err(|source| DriverError::Write {
                path: traces.clone(),
                source,
            })?;
        }
        let mut command = self.clang(dir);
        command.args(self.compile_flags(dir));
        match kind {
            Output::Object => drop(command.arg("-c")),
            Output::Assembly => drop(command.arg("-S")),
            Output::Executable => {
                self.check_link_inputs()?;
                command.args(self.link_flags());
            }
            Output::Module => {
                self.check_link_inputs()?;
                command.args(["-fPIC", "-shared"]);
                command.args(self.link_flags());
            }
        }
        command.arg("unit.c");
        if matches!(kind, Output::Executable | Output::Module) {
            command.args(link_with);
        }
        command.arg("-o").arg(&output);
        self.run(command).map(drop)
    }
}
