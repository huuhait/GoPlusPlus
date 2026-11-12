//! Symbol name mangling and demangling (appendix A of the specification).

mod ident;
#[cfg(test)]
mod long;
mod node;
mod parse;
mod readable;
mod sema;
#[cfg(test)]
mod sema_symbols;
#[cfg(test)]
mod symbol_name;
mod write;

pub use node::{BadNode, Basic, Context, Dir, Node, NodeId, Pair, Required, Wrap};
pub use parse::{ParseError, parse};
pub use sema::{Entity, Locals, Namer, NoSymbol, symbol_of};

/// What every Go++ symbol starts with.
pub const PREFIX: &str = "_GP";

/// How long a readable name may get unless more is asked for.
pub const MAX_OUTPUT: usize = 1 << 20;

/// Whether the name is in the namespace of Go++ symbols, which no name
/// that C sees may be in.
pub fn is_reserved_name(name: &str) -> bool {
    name.starts_with(PREFIX)
}

/// The symbol of the function `name` of the package at `path`.
pub fn func_symbol(path: &str, name: &str) -> String {
    let (package, name) = (path.as_bytes().into(), name.as_bytes().into());
    leaf_symbol(&Node::Func { package, name })
}

/// The symbol of the `init` function of the package at `path` with the
/// number, from 0.
pub fn init_symbol(path: &str, ordinal: u64) -> String {
    let package = path.as_bytes().into();
    leaf_symbol(&Node::InitFunc { package, ordinal })
}

/// The symbol of an entity without children: all of it is its head.
fn leaf_symbol(node: &Node) -> String {
    let mut symbol = String::from(PREFIX);
    node.head(&mut symbol);
    symbol
}

/// The symbol as Go++ text, cut after `at_most` bytes.
pub fn demangle(symbol: &[u8], at_most: usize) -> Result<String, ParseError> {
    let mut context = Context::new();
    let entity = parse(&mut context, symbol)?;
    Ok(context.readable(entity, at_most))
}

/// The text with every word that is a Go++ symbol as Go++ text.
pub fn filter(text: &str, at_most: usize) -> String {
    let in_word = |c: char| c.is_ascii_alphanumeric() || c == '_' || c == '$';
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    while !rest.is_empty() {
        let (word, after) = rest.split_at(rest.find(|c| !in_word(c)).unwrap_or(rest.len()));
        match demangle(word.as_bytes(), at_most) {
            Ok(readable) => out.push_str(&readable),
            Err(_) => out.push_str(word),
        }
        let (between, after) = after.split_at(after.find(in_word).unwrap_or(after.len()));
        out.push_str(between);
        rest = after;
    }
    out
}
