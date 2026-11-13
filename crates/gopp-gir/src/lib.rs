//! GIR: the typed mid-level IR, its builder, verifier, and safety passes.

mod builder;
mod ir;
mod lower;
mod print;
mod types;
mod verify;

pub use builder::{BuildError, FuncBuilder};
pub use ir::*;
pub use lower::{Checked, check_instances, lower_package};
pub use print::print_module;
pub use types::{FuncType, RecId, StructId, Type, TypeError, TypeId, TypeInterner};
pub use verify::{VerifyError, verify_module};

#[cfg(test)]
mod tests;
