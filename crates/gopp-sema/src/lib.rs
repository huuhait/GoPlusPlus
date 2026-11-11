//! Name resolution, type checking, constants, generics, and flow typing.

pub mod basic;
pub mod constant;
mod decls;
mod literal;
mod object;
mod package;
mod resolve;
mod typecheck;
pub mod types;
mod universe;

#[cfg(test)]
mod tests;

pub use decls::Initializer;
pub use object::{Body, FileInfo, Local, LocalId, LocalKind, ObjId, ObjKind, Object, Ref};
pub use package::{Package, PackageLoader};
pub use resolve::{Unit, check};
pub use universe::{Predeclared, PredeclaredKind};
