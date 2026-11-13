//! Byte and token mutation of existing sources.

use gopp_diag::Sink;
use gopp_span::{FileTooLarge, SourceFile, SourceMap};
use gopp_syntax::lexer;
use rand::{Rng, RngExt};

/// Spellings a byte edit rarely makes by chance.
const DICTIONARY: &[&str] = &[
    "break",
    "case",
    "chan",
    "const",
    "continue",
    "default",
    "defer",
    "else",
    "fallthrough",
    "for",
    "func",
    "go",
    "goto",
    "if",
    "import",
    "interface",
    "macro",
    "map",
    "package",
    "range",
    "return",
    "select",
    "struct",
    "switch",
    "type",
    "var",
    "move(",
    "@A",
    "@Extends(T)",
    "@Build(linux && !x)",
    "#[",
    "$x:expr",
    "f!(",
    "!",
    "<-",
    ":=",
    "...",
    "~",
    "|",
    "&^=",
    "<<=",
    "(",
    ")",
    "[",
    "]",
    "{",
    "}",
    ";",
    ",",
    ".",
    ":",
    "\n",
    "\"",
    "`",
    "'",
    "/*",
    "*/",
    "//",
    "0x",
    "1e",
    "0b2",
    "\\u",
    "x.(type)",
    "[]T{",
];

/// A source cut after each token, so that tokens can be removed, repeated
/// and reordered.
#[derive(Debug)]
pub struct Seed {
    pieces: Vec<String>,
}

impl Seed {
    pub fn new(text: String) -> Result<Self, FileTooLarge> {
        let map = SourceMap::new();
        let id = map.add(SourceFile::new("seed".into(), text)?);
        let source = map.file(id);
        let text = source.text();
        let mut pieces = Vec::new();
        let mut start = 0;
        for token in lexer::lex(id, &source, &Sink::new(0)).tokens {
            let end = token.hi as usize;
            if end > start {
                pieces.push(text[start..end].to_owned());
                start = end;
            }
        }
        if start < text.len() {
            pieces.push(text[start..].to_owned());
        }
        Ok(Self { pieces })
    }
}

/// A mutant of `seed`: up to three token edits, then up to two byte edits.
/// `donor` gives the tail of a splice.
pub fn mutate(seed: &Seed, donor: &Seed, rng: &mut impl Rng) -> Vec<u8> {
    let mut pieces: Vec<&str> = seed.pieces.iter().map(String::as_str).collect();
    for _ in 0..rng.random_range(0..=3) {
        edit_tokens(&mut pieces, donor, rng);
    }
    let mut bytes = pieces.concat().into_bytes();
    for _ in 0..rng.random_range(0..=2) {
        edit_bytes(&mut bytes, rng);
    }
    bytes
}

fn edit_tokens<'a>(pieces: &mut Vec<&'a str>, donor: &'a Seed, rng: &mut impl Rng) {
    let slot = rng.random_range(0..=pieces.len());
    let word = DICTIONARY[rng.random_range(0..DICTIONARY.len())];
    if pieces.is_empty() {
        pieces.push(word);
        return;
    }
    let last = pieces.len() - 1;
    let at = rng.random_range(0..=last);
    match rng.random_range(0..5) {
        0 => drop(pieces.remove(at)),
        1 => pieces.insert(slot, pieces[at]),
        2 => pieces.swap(at, slot.min(last)),
        3 => pieces.insert(slot, word),
        _ => {
            let tail = rng.random_range(0..=donor.pieces.len());
            pieces.truncate(slot);
            pieces.extend(donor.pieces[tail..].iter().map(String::as_str));
        }
    }
}

fn edit_bytes(bytes: &mut Vec<u8>, rng: &mut impl Rng) {
    let slot = rng.random_range(0..=bytes.len());
    if bytes.is_empty() {
        bytes.push(rng.random());
        return;
    }
    let at = rng.random_range(0..bytes.len());
    match rng.random_range(0..4) {
        0 => bytes[at] ^= 1 << rng.random_range(0..8),
        1 => bytes[at] = rng.random(),
        2 => bytes.insert(slot, rng.random()),
        _ => bytes.truncate(slot),
    }
}

#[cfg(test)]
mod tests {
    use rand::SeedableRng;
    use rand::rngs::ChaCha8Rng;

    use super::*;

    const SOURCE: &str =
        "package p\n\n// doc\nfunc f(x int) int {\n\treturn x + 1 // one\n}\n/* end */";

    fn seed(text: &str) -> Seed {
        Seed::new(text.to_owned()).unwrap()
    }

    #[test]
    fn the_pieces_are_the_whole_source() {
        let seed = seed(SOURCE);
        assert_eq!(seed.pieces.concat(), SOURCE);
        assert_eq!(seed.pieces[..3], ["package", " p", "\n"]);
        assert_eq!(seed.pieces.last().map(String::as_str), Some("/* end */"));
    }

    #[test]
    fn a_mutant_depends_only_on_the_seed_number() {
        let (seed, donor) = (seed(SOURCE), seed("package q\nvar v = []int{1, 2}\n"));
        let mutants = |number| {
            let mut rng = ChaCha8Rng::seed_from_u64(number);
            let mut mutant = || mutate(&seed, &donor, &mut rng);
            [mutant(), mutant(), mutant()]
        };
        assert_eq!(mutants(7), mutants(7));
        assert_ne!(mutants(7), mutants(8));
    }

    #[test]
    fn mutants_differ_from_the_seed_and_from_each_other() {
        let (seed, donor) = (seed(SOURCE), seed("package q\nvar v = []int{1, 2}\n"));
        let mut rng = ChaCha8Rng::seed_from_u64(1);
        let mutants: std::collections::BTreeSet<_> =
            (0..200).map(|_| mutate(&seed, &donor, &mut rng)).collect();
        assert!(
            mutants.len() > 150,
            "only {} distinct mutants",
            mutants.len()
        );
        assert!(mutants.iter().any(|mutant| mutant.len() < SOURCE.len() / 2));
        assert!(mutants.iter().any(|mutant| mutant.len() > SOURCE.len()));
        assert!(mutants.iter().any(|mutant| str::from_utf8(mutant).is_err()));
        assert!(mutants.iter().any(|mutant| mutant.ends_with(b"{1, 2}\n")));
    }

    #[test]
    fn an_empty_seed_still_mutates() {
        let empty = seed("");
        let mut rng = ChaCha8Rng::seed_from_u64(3);
        let mutants: Vec<_> = (0..50).map(|_| mutate(&empty, &empty, &mut rng)).collect();
        assert!(mutants.iter().any(|mutant| !mutant.is_empty()));
    }
}
