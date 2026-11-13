//! Name resolution, type checking, constants, generics, and flow typing.

pub mod basic;
pub mod constant;
mod consteval;
mod decls;
mod exprstring;
mod foreign;
mod info;
mod instance;
mod literal;
mod object;
mod package;
mod resolve;
mod typecheck;
pub mod types;
mod universe;

#[cfg(test)]
mod tests;

pub use consteval::{ConstCall, ConstEvaluator, ConstFailure};
pub use decls::Initializer;
pub use foreign::{ForeignMember, ForeignPackage, TemplateArg};
pub use info::info_text;
pub use instance::Written;
pub use object::{
    Bang, Body, Capture, CaptureUse, ExprType, FileInfo, ForeignUse, Instance, Local, LocalId,
    LocalKind, MethodValue, ObjId, ObjKind, Object, OperatorUse, Ref, Selection, SelectionKind,
    Transfer,
};
pub use package::{Package, PackageLoader};
pub use resolve::{Unit, check};
pub use universe::{Predeclared, PredeclaredKind};
