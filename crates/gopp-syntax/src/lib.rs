//! Lexer, parser, AST, and source printer.

pub mod ast;
pub mod constraint;
pub mod fmt;
pub mod lexer;
pub mod parser;
pub mod token;

/// Runs `f` on a stack that grows on demand, so recursion over nested source
/// cannot overflow it.
fn with_stack<R>(f: impl FnOnce() -> R) -> R {
    stacker::maybe_grow(128 * 1024, 2 * 1024 * 1024, f)
}
