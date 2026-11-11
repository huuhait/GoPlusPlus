//! Which files and declarations a build takes.
//!
//! A `.gopp` file says it with the attribute `@Build(condition)`: above the
//! package clause for the whole file, or on one declaration. A `.go` file
//! says it as Go does, with a `//go:build` line (or `// +build` lines) in
//! its header. For both, a system name at the end of the file's name counts.

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{FileId, SourceFile, Span};
use la_arena::Arena;

use crate::ast::{self, Attribute, BinaryOp, ExprId, ExprKind, File, UnaryOp};
use crate::with_stack;

/// Go refuses a `//go:build` line with more terms than this.
const MAX_TERMS: usize = 1000;
const MAX_PLUS_TERMS: usize = 100;
/// The Go release the language is based on: `go1.1` to `go1.25` hold.
const GO_RELEASE: u32 = 25;

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

/// Go's list, and `wasm32`, which is this compiler's name for `wasm`.
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
            || is_go_release(word)
    }

    fn knows(&self, word: &str) -> bool {
        self.holds(word)
            || word == "unix"
            || KNOWN_OS.contains(&word)
            || KNOWN_ARCH.contains(&word)
            || self.known.iter().any(|known| known == word)
    }

    /// Go's rule for `name_GOOS`, `name_GOARCH` and `name_GOOS_GOARCH` before
    /// the extension, with `_test` after them: such a file is for that
    /// system only.
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

fn is_go_release(word: &str) -> bool {
    let minor = word
        .strip_prefix("go1.")
        .and_then(|n| n.parse::<u32>().ok());
    minor.is_some_and(|minor| (1..=GO_RELEASE).contains(&minor))
}

/// Whether the file holds tests: its name ends in `_test` before the
/// extension.
pub fn is_test_file(name: &str) -> bool {
    let stem = name.rsplit_once('.').map_or(name, |(stem, _)| stem);
    stem.ends_with("_test")
}

// ---- `@Build`

/// Whether `attr` is `@Build`.
pub(crate) fn is_build(attr: &Attribute, text: &str) -> bool {
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

// ---- the lines of a `.go` file

/// The condition of a `//go:build` or `// +build` line.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Expr {
    Tag(String),
    Not(Box<Expr>),
    And(Box<Expr>, Box<Expr>),
    Or(Box<Expr>, Box<Expr>),
}

impl Expr {
    /// The value of the condition. Both sides of `&&` and `||` are asked,
    /// as Go does, so that `holds` sees every tag.
    pub fn eval(&self, holds: &mut impl FnMut(&str) -> bool) -> bool {
        match self {
            Self::Tag(tag) => holds(tag),
            Self::Not(x) => !x.eval(holds),
            Self::And(x, y) => {
                let (x, y) = (x.eval(holds), y.eval(holds));
                x && y
            }
            Self::Or(x, y) => {
                let (x, y) = (x.eval(holds), y.eval(holds));
                x || y
            }
        }
    }

    /// The condition as `gofmt` writes it.
    pub(crate) fn text(&self) -> String {
        let wrapped = |x: &Expr, wrap: bool| {
            if wrap {
                format!("({})", x.text())
            } else {
                x.text()
            }
        };
        match self {
            Self::Tag(tag) => tag.clone(),
            Self::Not(x) => {
                let wrap = matches!(**x, Self::And(..) | Self::Or(..));
                format!("!{}", wrapped(x, wrap))
            }
            Self::And(x, y) => {
                let or = |x: &Expr| matches!(x, Self::Or(..));
                format!("{} && {}", wrapped(x, or(x)), wrapped(y, or(y)))
            }
            Self::Or(x, y) => {
                let and = |x: &Expr| matches!(x, Self::And(..));
                format!("{} || {}", wrapped(x, and(x)), wrapped(y, and(y)))
            }
        }
    }
}

/// What is wrong with a condition, with Go's wording, and the byte in the
/// condition where.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SyntaxError {
    pub offset: usize,
    pub message: String,
}

const UNEXPECTED_END: &str = "unexpected end of expression";
const MISSING_PAREN: &str = "missing close paren";

type Parsed<T> = Result<T, SyntaxError>;

fn error<T>(offset: usize, message: impl Into<String>) -> Parsed<T> {
    Err(SyntaxError {
        offset,
        message: message.into(),
    })
}

/// A character of a tag. Go takes letters and decimal digits of every
/// script; of the digits, only the ASCII ones are taken here.
fn is_tag_char(c: char) -> bool {
    c.is_alphabetic() || c.is_ascii_digit() || c == '_' || c == '.'
}

struct Parser<'a> {
    text: &'a str,
    /// Where the next token starts.
    next: usize,
    token: &'a str,
    /// Where `token` starts.
    pos: usize,
    is_tag: bool,
    terms: usize,
}

impl<'a> Parser<'a> {
    fn new(text: &'a str) -> Self {
        Self {
            text,
            next: 0,
            token: "",
            pos: 0,
            is_tag: false,
            terms: 0,
        }
    }

    /// Reads the next token; at the end of the text it is empty.
    fn lex(&mut self) -> Parsed<()> {
        self.is_tag = false;
        let rest = &self.text[self.next..];
        self.next += rest.len() - rest.trim_start_matches([' ', '\t']).len();
        let rest = &self.text[self.next..];
        self.pos = self.next;
        let Some(first) = rest.chars().next() else {
            self.token = "";
            return Ok(());
        };
        let length = match first {
            '(' | ')' | '!' => 1,
            '&' | '|' if rest[1..].starts_with(first) => 2,
            _ => {
                let end = rest.find(|c| !is_tag_char(c));
                let length = end.unwrap_or(rest.len());
                if length == 0 {
                    return error(self.pos, format!("invalid syntax at {first}"));
                }
                self.is_tag = true;
                length
            }
        };
        self.token = &rest[..length];
        self.next += length;
        Ok(())
    }

    fn or(&mut self) -> Parsed<Expr> {
        let mut x = self.and()?;
        while self.token == "||" {
            x = Expr::Or(Box::new(x), Box::new(self.and()?));
        }
        Ok(x)
    }

    fn and(&mut self) -> Parsed<Expr> {
        let mut x = self.not()?;
        while self.token == "&&" {
            x = Expr::And(Box::new(x), Box::new(self.not()?));
        }
        Ok(x)
    }

    fn not(&mut self) -> Parsed<Expr> {
        self.terms += 1;
        if self.terms > MAX_TERMS {
            return error(self.pos, "build expression too large");
        }
        self.lex()?;
        if self.token == "!" {
            self.lex()?;
            if self.token == "!" {
                return error(self.pos, "double negation not allowed");
            }
            return Ok(Expr::Not(Box::new(self.atom()?)));
        }
        self.atom()
    }

    fn atom(&mut self) -> Parsed<Expr> {
        if self.token == "(" {
            let open = self.pos;
            let x = with_stack(|| self.or()).map_err(|mut error| {
                if error.message == UNEXPECTED_END {
                    error.message = MISSING_PAREN.to_owned();
                }
                error
            })?;
            if self.token != ")" {
                return error(open, MISSING_PAREN);
            }
            self.lex()?;
            return Ok(x);
        }
        if !self.is_tag {
            if self.token.is_empty() {
                return error(self.pos, UNEXPECTED_END);
            }
            return error(self.pos, format!("unexpected token {}", self.token));
        }
        let tag = self.token.to_owned();
        self.lex()?;
        Ok(Expr::Tag(tag))
    }
}

/// The condition after `//go:build`, read as Go reads it.
pub fn parse(text: &str) -> Result<Expr, SyntaxError> {
    let mut parser = Parser::new(text);
    let x = parser.or()?;
    if !parser.token.is_empty() {
        return error(parser.pos, format!("unexpected token {}", parser.token));
    }
    Ok(x)
}

/// The condition after `// +build`: blanks separate alternatives, commas
/// join what must hold together. What cannot be a tag reads as the tag
/// `ignore`. `None` if the line has too many terms.
fn parse_plus_build(text: &str) -> Option<Expr> {
    let ignore = || Expr::Tag("ignore".to_owned());
    let mut terms = 0;
    let mut x: Option<Expr> = None;
    for clause in text.split_whitespace() {
        let mut y: Option<Expr> = None;
        for literal in clause.split(',') {
            let z = if literal.starts_with("!!") || literal == "!" {
                ignore()
            } else {
                let (negated, tag) = match literal.strip_prefix('!') {
                    Some(tag) => (true, tag),
                    None => (false, literal),
                };
                let valid = !tag.is_empty() && tag.chars().all(is_tag_char);
                let z = if valid {
                    Expr::Tag(tag.to_owned())
                } else {
                    ignore()
                };
                if negated { Expr::Not(Box::new(z)) } else { z }
            };
            y = Some(match y {
                None => z,
                Some(y) => {
                    terms += 1;
                    if terms > MAX_PLUS_TERMS {
                        return None;
                    }
                    Expr::And(Box::new(y), Box::new(z))
                }
            });
        }
        let y = y?;
        x = Some(match x {
            None => y,
            Some(x) => {
                terms += 1;
                if terms > MAX_PLUS_TERMS {
                    return None;
                }
                Expr::Or(Box::new(x), Box::new(y))
            }
        });
    }
    Some(x.unwrap_or_else(ignore))
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

/// The condition of a `// +build` line.
fn plus_build_text(line: &str) -> Option<&str> {
    if line.contains('\n') {
        return None;
    }
    let rest = line.strip_prefix("//")?.trim().strip_prefix("+build")?;
    let trimmed = rest.trim_start();
    (trimmed.len() != rest.len() || rest.is_empty()).then_some(trimmed)
}

pub(crate) fn is_go_build(line: &str) -> bool {
    go_build_text(line).is_some()
}

pub(crate) fn is_plus_build(line: &str) -> bool {
    plus_build_text(line).is_some()
}

/// The condition of a `//go:build` or `// +build` line; `None` if the line
/// is neither or its condition is not well formed.
pub(crate) fn parse_line(line: &str) -> Option<Expr> {
    if let Some(text) = go_build_text(line) {
        return parse(text).ok();
    }
    parse_plus_build(plus_build_text(line)?)
}

/// The condition in the header of a `.go` file, found as Go's `go/build`
/// finds it: the header is the comments and blank lines above the package
/// clause. A `//go:build` line counts anywhere in it, outside a `/* */`
/// comment. Without one, the `// +build` lines count that stand before the
/// last blank line of the header, and all of them must hold. `None` if the
/// header has no condition.
pub fn go_header(file: FileId, source: &SourceFile) -> Result<Option<Expr>, Diagnostic> {
    let text = source.text();
    let mut go_build: Option<(usize, &str)> = None;
    let mut in_block = false;
    let mut past_line_comments = false;
    let mut plus_build_end = 0;
    let mut offset = 0;
    'lines: for raw in text.split_inclusive('\n') {
        let start = offset;
        offset += raw.len();
        let line = raw.trim();
        if line.is_empty() && !past_line_comments {
            plus_build_end = offset;
            continue;
        }
        past_line_comments |= !line.starts_with("//");
        if !in_block && is_go_build(line) {
            let lo = start + raw.len() - raw.trim_start().len();
            if go_build.is_some() {
                let span = span(file, lo, line.len());
                let message = "multiple //go:build comments";
                return Err(Diagnostic::new(Code::BadBuildConstraint, span, message));
            }
            go_build = Some((lo, line));
        }
        let mut rest = line;
        while !rest.is_empty() {
            if in_block {
                let Some(end) = rest.find("*/") else {
                    continue 'lines;
                };
                in_block = false;
                rest = rest[end + 2..].trim();
            } else if rest.starts_with("//") {
                continue 'lines;
            } else if let Some(inside) = rest.strip_prefix("/*") {
                in_block = true;
                rest = inside.trim();
            } else {
                break 'lines;
            }
        }
    }

    if let Some((lo, line)) = go_build {
        let condition = go_build_text(line).unwrap_or_default();
        return match parse(condition) {
            Ok(x) => Ok(Some(x)),
            Err(error) => {
                let message = format!("parsing //go:build line: {}", error.message);
                let span = span(file, lo, line.len());
                Err(Diagnostic::new(Code::BadBuildConstraint, span, message))
            }
        };
    }
    let lines = text[..plus_build_end].lines();
    let conditions = lines.filter_map(|line| parse_plus_build(plus_build_text(line.trim())?));
    Ok(conditions.reduce(|all, x| Expr::And(Box::new(all), Box::new(x))))
}

fn span(file: FileId, lo: usize, length: usize) -> Span {
    Span {
        file,
        lo: lo as u32,
        hi: (lo + length) as u32,
    }
}

#[cfg(test)]
mod tests;
