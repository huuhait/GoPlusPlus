//! What the compiler reports to the user.

use std::fmt;

use gopp_span::Span;

use crate::Code;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Severity {
    Error,
    Warning,
    Note,
}

impl fmt::Display for Severity {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Error => "error",
            Self::Warning => "warning",
            Self::Note => "note",
        })
    }
}

/// An error or a warning at one place in the source.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    pub code: Code,
    pub message: String,
    pub span: Span,
    pub notes: Vec<Note>,
}

/// More about the diagnostic it belongs to, with or without a place of its own.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Note {
    pub code: Code,
    pub message: String,
    pub span: Option<Span>,
}

impl Diagnostic {
    pub fn new(code: Code, span: Span, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
            span,
            notes: Vec::new(),
        }
    }

    pub fn with_note(mut self, code: Code, span: Option<Span>, message: impl Into<String>) -> Self {
        self.notes.push(Note {
            code,
            message: message.into(),
            span,
        });
        self
    }
}
