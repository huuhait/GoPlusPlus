//! `//go:build` and `// +build` lines: `gofmt` moves them to the top of
//! the file, writes the constraint in its canonical form, and keeps the
//! `// +build` lines of a file in step with its `//go:build` line.

use super::tabwriter::ESCAPE;

const FORMFEED: u8 = 0x0C;
/// Constraints with more terms than this are left as they are.
const MAX_TERMS: usize = 1000;
const MAX_PLUS_TERMS: usize = 100;

#[derive(Clone, Debug, PartialEq, Eq)]
enum Expr {
    Tag(String),
    Not(Box<Expr>),
    And(Box<Expr>, Box<Expr>),
    Or(Box<Expr>, Box<Expr>),
}

impl Expr {
    fn text(&self) -> String {
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

/// The constraint of a `//go:build` line.
fn go_build_text(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("//go:build")?;
    if line.contains('\n') {
        return None;
    }
    let rest = rest.trim_end();
    let trimmed = rest.trim_start();
    (trimmed.len() != rest.len() || rest.is_empty()).then_some(trimmed)
}

/// The constraint of a `// +build` line.
fn plus_build_text(line: &str) -> Option<&str> {
    if line.contains('\n') {
        return None;
    }
    let rest = line.strip_prefix("//")?.trim().strip_prefix("+build")?;
    let trimmed = rest.trim_start();
    (trimmed.len() != rest.len() || rest.is_empty()).then_some(trimmed)
}

pub(super) fn is_go_build(line: &str) -> bool {
    go_build_text(line).is_some()
}

pub(super) fn is_plus_build(line: &str) -> bool {
    plus_build_text(line).is_some()
}

fn is_tag_char(c: char) -> bool {
    c.is_alphanumeric() || c == '_' || c == '.'
}

struct Parser<'a> {
    rest: &'a str,
    token: &'a str,
    is_tag: bool,
    terms: usize,
}

impl<'a> Parser<'a> {
    fn lex(&mut self) -> Option<()> {
        self.is_tag = false;
        self.rest = self.rest.trim_start_matches([' ', '\t']);
        let Some(first) = self.rest.chars().next() else {
            self.token = "";
            return Some(());
        };
        let length = match first {
            '(' | ')' | '!' => 1,
            '&' | '|' => {
                if !self.rest[1..].starts_with(first) {
                    return None;
                }
                2
            }
            _ => {
                let end = self.rest.find(|c| !is_tag_char(c));
                let length = end.unwrap_or(self.rest.len());
                if length == 0 {
                    return None;
                }
                self.is_tag = true;
                length
            }
        };
        (self.token, self.rest) = self.rest.split_at(length);
        Some(())
    }

    fn or(&mut self) -> Option<Expr> {
        let mut x = self.and()?;
        while self.token == "||" {
            x = Expr::Or(Box::new(x), Box::new(self.and()?));
        }
        Some(x)
    }

    fn and(&mut self) -> Option<Expr> {
        let mut x = self.not()?;
        while self.token == "&&" {
            x = Expr::And(Box::new(x), Box::new(self.not()?));
        }
        Some(x)
    }

    fn not(&mut self) -> Option<Expr> {
        self.terms += 1;
        if self.terms > MAX_TERMS {
            return None;
        }
        self.lex()?;
        if self.token == "!" {
            self.lex()?;
            if self.token == "!" {
                return None;
            }
            return Some(Expr::Not(Box::new(self.atom()?)));
        }
        self.atom()
    }

    fn atom(&mut self) -> Option<Expr> {
        if self.token == "(" {
            let x = self.or()?;
            if self.token != ")" {
                return None;
            }
            self.lex()?;
            return Some(x);
        }
        if !self.is_tag {
            return None;
        }
        let tag = self.token.to_owned();
        self.lex()?;
        Some(Expr::Tag(tag))
    }
}

fn parse_go_build(text: &str) -> Option<Expr> {
    let mut parser = Parser {
        rest: text,
        token: "",
        is_tag: false,
        terms: 0,
    };
    let x = parser.or()?;
    parser.token.is_empty().then_some(x)
}

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

/// The constraint of a `//go:build` or `// +build` line.
fn parse(line: &str) -> Option<Expr> {
    if let Some(text) = go_build_text(line) {
        return parse_go_build(text);
    }
    parse_plus_build(plus_build_text(line)?)
}

/// `x` with every negation moved down to a tag.
fn push_not(x: &Expr, negate: bool) -> Expr {
    match x {
        Expr::Not(inner) if matches!(**inner, Expr::Tag(_)) && !negate => x.clone(),
        Expr::Not(inner) => push_not(inner, !negate),
        Expr::Tag(_) if negate => Expr::Not(Box::new(x.clone())),
        Expr::Tag(_) => x.clone(),
        Expr::And(a, b) | Expr::Or(a, b) => {
            let (a, b) = (Box::new(push_not(a, negate)), Box::new(push_not(b, negate)));
            if matches!(x, Expr::And(..)) != negate {
                Expr::And(a, b)
            } else {
                Expr::Or(a, b)
            }
        }
    }
}

fn split_and<'x>(x: &'x Expr, list: &mut Vec<&'x Expr>) {
    match x {
        Expr::And(a, b) => {
            split_and(a, list);
            split_and(b, list);
        }
        _ => list.push(x),
    }
}

fn split_or<'x>(x: &'x Expr, list: &mut Vec<&'x Expr>) {
    match x {
        Expr::Or(a, b) => {
            split_or(a, list);
            split_or(b, list);
        }
        _ => list.push(x),
    }
}

/// The `// +build` lines that say the same as `x`, or why there are none.
fn plus_build_lines(x: &Expr) -> Result<Vec<String>, &'static str> {
    let x = push_not(x, false);
    let mut lines: Vec<Vec<Vec<&Expr>>> = Vec::new();
    let mut conjuncts = Vec::new();
    split_and(&x, &mut conjuncts);
    for conjunct in conjuncts {
        let mut clauses = Vec::new();
        let mut alternatives = Vec::new();
        split_or(conjunct, &mut alternatives);
        for alternative in alternatives {
            let mut literals = Vec::new();
            split_and(alternative, &mut literals);
            let simple = |x: &&Expr| matches!(x, Expr::Tag(_) | Expr::Not(_));
            if !literals.iter().all(simple) {
                return Err("expression too complex for // +build lines");
            }
            clauses.push(literals);
        }
        lines.push(clauses);
    }
    // Lines of one clause each say "and" as well as one line does.
    if lines.iter().all(|clauses| clauses.len() == 1) {
        let literals = lines.into_iter().flatten().flatten().collect();
        lines = vec![vec![literals]];
    }
    let line_text = |clauses: &Vec<Vec<&Expr>>| {
        let clause_text = |literals: &Vec<&Expr>| {
            let texts: Vec<String> = literals.iter().map(|x| x.text()).collect();
            format!(" {}", texts.join(","))
        };
        let clauses: String = clauses.iter().map(clause_text).collect();
        format!("// +build{clauses}")
    };
    Ok(lines.iter().map(line_text).collect())
}

fn is_newline(byte: u8) -> bool {
    byte == b'\n' || byte == FORMFEED
}

/// The line that starts at `start`, with its line end.
fn line_at(output: &[u8], start: usize) -> &[u8] {
    let rest = &output[start..];
    let end = rest.iter().position(|&byte| is_newline(byte));
    &rest[..end.map_or(rest.len(), |end| end + 1)]
}

fn comment_text_at(output: &[u8], mut start: usize) -> String {
    if output.get(start) == Some(&ESCAPE) {
        start += 1;
    }
    let rest = &output[start..];
    let end = rest.iter().position(|&b| b == ESCAPE || is_newline(b));
    String::from_utf8_lossy(&rest[..end.unwrap_or(rest.len())]).into_owned()
}

/// Appends whole lines, without making two blank lines in a row.
fn append_lines(to: &mut Vec<u8>, mut lines: &[u8]) {
    let ends_blank = to.is_empty()
        || to.len() >= 2 && is_newline(to[to.len() - 1]) && is_newline(to[to.len() - 2]);
    if lines.first().is_some_and(|&byte| is_newline(byte)) && ends_blank {
        lines = &lines[1..];
    }
    to.extend_from_slice(lines);
}

/// Rewrites the build lines in the printer's text, which is not laid out
/// yet: comments are still between escape bytes. `go_build` and
/// `plus_build` are where those lines start.
pub(super) fn fix_lines(output: &mut Vec<u8>, go_build: &[usize], plus_build: &[usize]) {
    if go_build.is_empty() && plus_build.is_empty() {
        return;
    }

    // The lines go after the last blank line of the leading comments, or
    // where the first of them is, if that is earlier.
    let mut insert = 0;
    let mut pos = 0;
    loop {
        let mut blank = true;
        while output.get(pos).is_some_and(|&b| b == b' ' || b == b'\t') {
            pos += 1;
        }
        if pos + 3 < output.len() && output[pos..pos + 3] == [ESCAPE, b'/', b'/'] {
            blank = false;
            while pos < output.len() && !is_newline(output[pos]) {
                pos += 1;
            }
        }
        if pos >= output.len() || !is_newline(output[pos]) {
            break;
        }
        pos += 1;
        if blank {
            insert = pos;
        }
    }
    if let Some(&first) = go_build.first().filter(|&&first| first < insert) {
        insert = first;
    } else if let Some(&first) = plus_build.first().filter(|&&first| first < insert) {
        insert = first;
    }

    let constraint = match go_build {
        [] => {
            let parsed = plus_build
                .iter()
                .map(|&start| parse(&comment_text_at(output, start)));
            let parsed: Option<Vec<Expr>> = parsed.collect();
            let all = |list: Vec<Expr>| {
                list.into_iter()
                    .reduce(|x, y| Expr::And(Box::new(x), Box::new(y)))
            };
            parsed.and_then(all)
        }
        [only] => parse(&comment_text_at(output, *only)),
        _ => None,
    };

    let mut block = Vec::new();
    match constraint {
        None => {
            for &start in go_build.iter().chain(plus_build) {
                block.extend_from_slice(line_at(output, start));
            }
        }
        Some(x) => {
            let mut lines = vec![format!("//go:build {}", x.text())];
            if !plus_build.is_empty() {
                match plus_build_lines(&x) {
                    Ok(plus) => lines.extend(plus),
                    Err(error) => lines.push(format!("// +build error: {error}")),
                }
            }
            for line in lines {
                block.push(ESCAPE);
                block.extend_from_slice(line.as_bytes());
                block.extend_from_slice(&[ESCAPE, b'\n']);
            }
        }
    }
    block.push(b'\n');

    let mut delete: Vec<usize> = go_build.iter().chain(plus_build).copied().collect();
    delete.sort_unstable();
    let mut after = Vec::new();
    let mut start = insert;
    for end in delete {
        if end < start {
            continue;
        }
        append_lines(&mut after, &output[start..end]);
        start = end + line_at(output, end).len();
    }
    append_lines(&mut after, &output[start..]);
    if after.len() >= 2 && is_newline(after[after.len() - 1]) && is_newline(after[after.len() - 2])
    {
        after.pop();
    }

    output.truncate(insert);
    output.extend_from_slice(&block);
    output.extend_from_slice(&after);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn canonical(line: &str) -> Option<String> {
        parse(line).map(|x| x.text())
    }

    #[test]
    fn build_lines_are_recognized() {
        assert!(is_go_build("//go:build linux"));
        assert!(is_go_build("//go:build"));
        assert!(!is_go_build("//go:buildx"));
        assert!(!is_go_build("// go:build linux"));
        assert!(is_plus_build("// +build linux"));
        assert!(is_plus_build("//+build linux"));
        assert!(!is_plus_build("// +builder"));
    }

    #[test]
    fn a_constraint_is_written_in_canonical_form() {
        assert_eq!(
            canonical("//go:build  (linux&&!cgo)||darwin").as_deref(),
            Some("(linux && !cgo) || darwin")
        );
        assert_eq!(
            canonical("//go:build !(a || b) && c").as_deref(),
            Some("!(a || b) && c")
        );
        assert_eq!(canonical("//go:build a &&"), None);
        assert_eq!(canonical("//go:build !!a"), None);
        assert_eq!(
            canonical("// +build linux,!cgo darwin").as_deref(),
            Some("(linux && !cgo) || darwin")
        );
    }

    #[test]
    fn plus_build_lines_say_the_same_as_the_constraint() {
        let lines = |text: &str| plus_build_lines(&parse_go_build(text).unwrap());
        assert_eq!(
            lines("(linux && !cgo) || darwin").unwrap(),
            ["// +build linux,!cgo darwin"]
        );
        assert_eq!(
            lines("(a || b) && c").unwrap(),
            ["// +build a b", "// +build c"]
        );
        assert_eq!(lines("a && b").unwrap(), ["// +build a,b"]);
        assert!(lines("(a && (b || c)) || d").is_err());
    }
}
