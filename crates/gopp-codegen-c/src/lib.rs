//! Lowers GIR to GNU C23 or C++20 and drives Clang.

pub mod driver;
mod emit;
pub mod runtime;
pub mod toolchain;

pub use emit::{EmitError, EmitOptions, Entry, emit_c23};
