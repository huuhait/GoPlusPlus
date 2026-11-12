//! A C or C++ header as a package (FOR-1 to FOR-3). The checker does not
//! read headers: it asks what did for each name that a program uses.

use std::fmt::Debug;

use lasso::ThreadedRodeo;

use crate::constant::Value;
use crate::types::{TypeId, Types};

/// What a header declares with one name.
#[derive(Clone, Debug, PartialEq)]
pub enum ForeignMember {
    Func {
        /// The type of the function in Go++.
        sig: TypeId,
        /// For each parameter, whether it is a pointer or a reference to
        /// what the function does not change (`const T*`, ADR-0008).
        read_only: Vec<bool>,
    },
    Var {
        ty: TypeId,
        /// Declared `const`: it is not assigned to.
        read_only: bool,
    },
    /// An enumerator, or a macro that is a constant.
    Const {
        ty: TypeId,
        value: Value,
    },
    Type(TypeId),
    /// A template: what it is depends on its arguments (ADR-0010).
    Template,
}

/// An argument that is written for a template in brackets.
#[derive(Clone, Debug, PartialEq)]
pub enum TemplateArg {
    Type(TypeId),
    /// A constant, with its type; one without a type takes the type of
    /// the template parameter.
    Const(Value, TypeId),
}

pub trait ForeignPackage: Debug + Send + Sync {
    /// What the header declares with the name, as Go++ spells it: a
    /// keyword has an underscore after it. `None` if it declares nothing
    /// of that name. The types are made in `types`.
    fn member(&self, name: &str, types: &Types, names: &ThreadedRodeo) -> Option<ForeignMember>;

    /// The template `name` with the template arguments `explicit`, the
    /// first ones, and for a call the types of the arguments `args`, from
    /// which the other template arguments are found; a constant without a
    /// type has the untyped type of its kind. `Err` says why there is no
    /// such instance. `types` may be a copy of the table that one worker
    /// has to itself: no type made for one call is kept for another.
    fn instance(
        &self,
        name: &str,
        explicit: &[TemplateArg],
        args: Option<&[TypeId]>,
        types: &Types,
        names: &ThreadedRodeo,
    ) -> Result<ForeignMember, String>;
}
