//! GIR: the typed mid-level IR, its builder, verifier, and safety passes.

mod builder;
mod ctype;
mod inline;
mod ir;
mod lower;
mod print;
mod types;
mod verify;

pub use builder::{BuildError, FuncBuilder};
pub use ctype::{CRefusal, CTypeRules, MAX_BYTES, MAX_DEPTH, MAX_FIELDS};
pub use inline::{MAX_STATEMENTS, inline_trivial};
pub use ir::*;
pub use lower::{Checked, Imported, Summaries, check_instances, lower_package, lower_with};
pub use print::print_module;
pub use types::{FuncType, RecId, StructId, Type, TypeError, TypeId, TypeInterner};
pub use verify::{VerifyError, verify_module};

#[cfg(test)]
mod tests;
