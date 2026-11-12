//! Diagnostics: the error-code registry, rendering, and explanations.

mod code;
mod diagnostic;
mod explain;
mod lock;
mod render;
mod sink;

pub use code::Code;
pub use diagnostic::{Diagnostic, Note, Severity};
pub use explain::{Explanation, Status, explain, explained, listed, status};
pub use lock::{LockError, LockRegistry};
pub use render::{render_json, render_text};
pub use sink::Sink;
