//! GIR: the typed mid-level IR, its builder, verifier, and safety passes.

mod builder;
mod ir;
mod print;
mod types;

pub use builder::{BuildError, FuncBuilder};
pub use ir::*;
pub use print::print_module;
pub use types::{FuncType, RecId, StructId, Type, TypeError, TypeId, TypeInterner};

#[cfg(test)]
mod tests;
