//! What runs a `@Const` function while the program is compiled. The
//! checker does not run code: the driver gives it what does (ADR-0033).

use lasso::Spur;

use crate::constant::Value;
use crate::types::TypeId;

/// A call of a `@Const` function or method with constant arguments.
#[derive(Debug)]
pub struct ConstCall<'c> {
    /// For a function, the import path of the package that declares it.
    pub package: Option<&'c str>,
    /// For a method, the type that declares it, with its type arguments.
    pub receiver: Option<TypeId>,
    pub name: Spur,
    /// The type of the function as it is called.
    pub sig: TypeId,
    /// The arguments, the receiver of a method first.
    pub args: &'c [Value],
}

/// Why a call gave no value: a panic, a division by zero, an index out of
/// range, a limit (EXP-34).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConstFailure {
    pub message: String,
    /// The functions that were running, the innermost first.
    pub stack: Vec<String>,
}

pub trait ConstEvaluator: Sync {
    /// What the function gives for the arguments.
    fn call(&self, call: &ConstCall<'_>) -> Result<Value, ConstFailure>;
}
