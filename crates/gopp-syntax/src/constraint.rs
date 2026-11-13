//! Which files and declarations a build takes.
//!
//! A file says it with the attribute `@Build(condition)`: above the package
//! clause for the whole file, or on one declaration. A system name at the
//! end of the file's name counts too.

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{SourceFile, Span};
use la_arena::Arena;

use crate::ast::{self, Attribute, BinaryOp, ExprId, ExprKind, File, UnaryOp};

const KNOWN_OS: &[&str] = &[
    "aix",
    "android",
    "darwin",
    "dragonfly",
    "freebsd",
    "hurd",
    "illumos",
    "ios",
    "js",
    "linux",
    "nacl",
    "netbsd",
    "openbsd",
    "plan9",
    "solaris",
    "wasip1",
    "windows",
    "zos",
];

const UNIX_OS: &[&str] = &[
    "aix",
    "android",
    "darwin",
    "dragonfly",
    "freebsd",
    "hurd",
    "illumos",
    "ios",
    "linux",
    "netbsd",
    "openbsd",
    "solaris",
];

/// The architectures that a build condition may name.
const KNOWN_ARCH: &[&str] = &[
    "386",
    "amd64",
    "amd64p32",
    "arm",
    "armbe",
    "arm64",
    "arm64be",
    "loong64",
    "mips",
    "mipsle",
    "mips64",
    "mips64le",
    "mips64p32",
    "mips64p32le",
    "ppc",
    "ppc64",
    "ppc64le",
    "riscv",
    "riscv64",
    "s390",
    "s390x",
    "sparc",
    "sparc64",
    "wasm",
    "wasm32",
];

/// What a build is for.
#[derive(Clone, Debug, Default)]
pub struct BuildContext {
    pub os: String,
    pub arch: String,
    /// The active build profile of the main module.
    pub profile: String,
    /// The tags that are on.
    pub tags: Vec<String>,
    /// The words the module being compiled may name besides system names:
    /// the tags it declares and every profile name, on or not.
    pub known: Vec<String>,
}

impl BuildContext {
    /// Whether `word` holds in this build. A word nobody knows does not.
    pub fn holds(&self, word: &str) -> bool {
        word == self.os
            || word == self.arch
            || word == self.profile
            || word == "unix" && UNIX_OS.contains(&self.os.as_str())
            || self.tags.iter().any(|tag| tag == word)
    }

    fn knows(&self, word: &str) -> bool {
        self.holds(word)
            || word == "unix"
            || KNOWN_OS.contains(&word)
            || KNOWN_ARCH.contains(&word)
            || self.known.iter().any(|known| known == word)
    }

    /// A file named `name_OS`, `name_ARCH` or `name_OS_ARCH` before the
    /// extension, with `_test` after them, is for that system only.
    pub fn takes_file_name(&self, name: &str) -> bool {
        let stem = name.split_once('.').map_or(name, |(stem, _)| stem);
        // What stands before the first `_` is never a system name.
        let Some(first) = stem.find('_') else {
            return true;
        };
        let mut parts: Vec<&str> = stem[first..].split('_').collect();
        if parts.last() == Some(&"test") {
            parts.pop();
        }
        match parts[..] {
            [.., os, arch] if KNOWN_OS.contains(&os) && KNOWN_ARCH.contains(&arch) => {
                self.holds(os) && self.holds(arch)
            }
            [.., last] if KNOWN_OS.contains(&last) || KNOWN_ARCH.contains(&last) => {
                self.holds(last)
            }
            _ => true,
        }
    }
}

/// Whether the file holds tests: its name ends in `_test` before the
/// extension.
pub fn is_test_file(name: &str) -> bool {
    let stem = name.rsplit_once('.').map_or(name, |(stem, _)| stem);
    stem.ends_with("_test")
}

// ---- `@Build`

/// Whether `attr` is `@Build`.
pub fn is_build(attr: &Attribute, text: &str) -> bool {
    let name = attr.path.name.span;
    attr.path.package.is_none() && &text[name.lo as usize..name.hi as usize] == "Build"
}

/// The value of the condition `x` of a `@Build`, given what each word is
/// worth. `Err` is the part that is no condition. Both sides of `&&` and
/// `||` are asked, so that `word` sees every word.
pub(crate) fn condition(
    exprs: &Arena<ast::Expr>,
    x: ExprId,
    word: &mut impl FnMut(Span) -> bool,
) -> Result<bool, Span> {
    let node = &exprs[x];
    Ok(match node.kind {
        ExprKind::Name(_) => word(node.span),
        ExprKind::Paren(inner) => condition(exprs, inner, word)?,
        ExprKind::Unary {
            op: UnaryOp::Not,
            x,
        } => !condition(exprs, x, word)?,
        ExprKind::Binary {
            op: op @ (BinaryOp::AndAnd | BinaryOp::OrOr),
            x,
            y,
        } => {
            let (x, y) = (condition(exprs, x, word)?, condition(exprs, y, word)?);
            if op == BinaryOp::AndAnd {
                x && y
            } else {
                x || y
            }
        }
        _ => return Err(node.span),
    })
}

/// Applies the `@Build` conditions of `file`: the declarations whose
/// condition does not hold leave the tree. False when the condition above
/// the package clause does not hold; the file is then no part of the build.
/// A word that `cx` does not know is an error.
pub fn apply(file: &mut File, source: &SourceFile, cx: &BuildContext, sink: &Sink) -> bool {
    let text = source.text();
    let holds = |attrs: &[Attribute], exprs: &Arena<ast::Expr>| {
        let mut all = true;
        for attr in attrs.iter().filter(|attr| is_build(attr, text)) {
            // The parser has reported any other shape.
            let Some(&[x]) = attr.args.as_deref() else {
                continue;
            };
            let mut word = |span: Span| {
                let word = &text[span.lo as usize..span.hi as usize];
                if !cx.knows(word) {
                    let message = format!(
                        "{word} is not a system, an architecture, a build profile or a tag of \
                         this module"
                    );
                    sink.push(Diagnostic::new(Code::BadBuildConstraint, span, message));
                }
                cx.holds(word)
            };
            all &= condition(exprs, x, &mut word).unwrap_or(true);
        }
        all
    };
    if !holds(&file.attrs, &file.exprs) {
        return false;
    }
    let File {
        decls,
        decl_nodes,
        exprs,
        ..
    } = file;
    decls.retain(|&decl| holds(&decl_nodes[decl].attrs, exprs));
    true
}

/// The condition of a `//go:build` line; `//go:builder` is none.
pub(crate) fn go_build_text(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("//go:build")?;
    if line.contains('\n') {
        return None;
    }
    let rest = rest.trim_end();
    let trimmed = rest.trim_start();
    (trimmed.len() != rest.len() || rest.is_empty()).then_some(trimmed)
}
