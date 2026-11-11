//! Discovery of the host Clang and LLD.

use std::env;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

/// The only Clang and LLD major `gopp` accepts.
pub const CLANG_MAJOR: u32 = 23;

#[derive(Debug)]
pub struct Tool {
    pub path: PathBuf,
    pub version: String,
}

#[derive(Debug)]
pub struct HostToolchain {
    pub clang: Tool,
    pub lld: Tool,
}

#[derive(Debug, thiserror::Error)]
pub enum ToolchainError {
    #[error("clang not found: set GOPP_CLANG or put clang {CLANG_MAJOR} on PATH")]
    ClangNotFound,
    #[error("ld.lld not found: {} cannot locate it", clang.display())]
    LldNotFound { clang: PathBuf },
    #[error("cannot run {}: {source}", path.display())]
    Run { path: PathBuf, source: io::Error },
    #[error("{} did not report a {name} version", path.display())]
    NoVersion { name: &'static str, path: PathBuf },
    #[error("{} is {name} {version}, but gopp needs {name} {CLANG_MAJOR}", path.display())]
    WrongMajor {
        name: &'static str,
        path: PathBuf,
        version: String,
    },
}

impl HostToolchain {
    /// Finds Clang through `GOPP_CLANG`, then `PATH`, and LLD through that Clang.
    pub fn discover() -> Result<Self, ToolchainError> {
        let clang = checked_tool("clang", "clang version ", find_clang()?)?;
        let reported = run(&clang.path, "-print-prog-name=ld.lld")?;
        let lld_path = PathBuf::from(reported.trim());
        if !lld_path.is_absolute() {
            return Err(ToolchainError::LldNotFound { clang: clang.path });
        }
        let lld = checked_tool("LLD", "LLD ", lld_path)?;
        Ok(Self { clang, lld })
    }
}

fn find_clang() -> Result<PathBuf, ToolchainError> {
    if let Some(path) = env::var_os("GOPP_CLANG").filter(|path| !path.is_empty()) {
        return Ok(path.into());
    }
    which::which(format!("clang-{CLANG_MAJOR}"))
        .or_else(|_| which::which("clang"))
        .map_err(|_| ToolchainError::ClangNotFound)
}

fn checked_tool(name: &'static str, marker: &str, path: PathBuf) -> Result<Tool, ToolchainError> {
    let output = run(&path, "--version")?;
    let Some(version) = version_after(&output, marker) else {
        return Err(ToolchainError::NoVersion { name, path });
    };
    let version = version.to_owned();
    if major(&version) != Some(CLANG_MAJOR) {
        return Err(ToolchainError::WrongMajor {
            name,
            path,
            version,
        });
    }
    Ok(Tool { path, version })
}

fn run(path: &Path, arg: &str) -> Result<String, ToolchainError> {
    let output = Command::new(path)
        .arg(arg)
        .output()
        .map_err(|source| ToolchainError::Run {
            path: path.to_owned(),
            source,
        })?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

/// The dotted number that follows `marker`, as in `clang version 23.1.0 (...)`.
fn version_after<'a>(output: &'a str, marker: &str) -> Option<&'a str> {
    let rest = &output[output.find(marker)? + marker.len()..];
    let end = rest
        .find(|c: char| !c.is_ascii_digit() && c != '.')
        .unwrap_or(rest.len());
    (end > 0).then(|| &rest[..end])
}

fn major(version: &str) -> Option<u32> {
    version.split('.').next()?.parse().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_follows_the_marker() {
        let nix = "clang version 23.1.0\nTarget: x86_64-unknown-linux-gnu\n";
        let debian = "Debian clang version 23.1.0 (++20261001-1)\n";
        let lld = "LLD 23.1.0 (compatible with GNU linkers)\n";
        assert_eq!(version_after(nix, "clang version "), Some("23.1.0"));
        assert_eq!(version_after(debian, "clang version "), Some("23.1.0"));
        assert_eq!(version_after(lld, "LLD "), Some("23.1.0"));
    }

    #[test]
    fn missing_version_is_none() {
        assert_eq!(version_after("gcc (GCC) 15.2.0", "clang version "), None);
        assert_eq!(
            version_after("clang version unknown", "clang version "),
            None
        );
    }

    #[test]
    fn major_is_the_first_component() {
        assert_eq!(major("23.1.0"), Some(23));
        assert_eq!(major("22"), Some(22));
        assert_eq!(major(".1"), None);
    }
}
