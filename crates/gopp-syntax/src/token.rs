//! Tokens and comments.

use logos::Logos;

use crate::lexer::{self, Scan};

/// A token's byte range in its file. `bad` marks a literal that was reported
/// as malformed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Token {
    pub kind: TokenKind,
    pub lo: u32,
    pub hi: u32,
    pub bad: bool,
}

/// A comment's byte range, delimiters included. `directive` marks a line
/// comment that starts with `//gopp:`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Comment {
    pub lo: u32,
    pub hi: u32,
    pub directive: bool,
}

#[derive(Logos, Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[logos(extras = Scan)]
#[logos(skip r"[ \t\r]+")]
pub enum TokenKind {
    /// Keywords match this pattern too; the callback tells them apart.
    #[regex(r"[A-Za-z_][A-Za-z0-9_]*", lexer::word)]
    #[regex(r"[^\x00-\x7F]", lexer::non_ascii)]
    Ident,
    #[regex(r"[0-9]", lexer::number)]
    #[regex(r"\.[0-9]", lexer::number)]
    Int,
    Float,
    Imag,
    #[token("'", lexer::rune)]
    Rune,
    #[token("\"", lexer::string)]
    #[token("`", lexer::raw_string)]
    String,

    #[token("+")]
    Plus,
    #[token("-")]
    Minus,
    #[token("*")]
    Star,
    #[token("/")]
    Slash,
    #[token("%")]
    Percent,
    #[token("&")]
    Amp,
    #[token("|")]
    Pipe,
    #[token("^")]
    Caret,
    #[token("<<")]
    Shl,
    #[token(">>")]
    Shr,
    #[token("&^")]
    AmpCaret,
    #[token("+=")]
    PlusEq,
    #[token("-=")]
    MinusEq,
    #[token("*=")]
    StarEq,
    #[token("/=")]
    SlashEq,
    #[token("%=")]
    PercentEq,
    #[token("&=")]
    AmpEq,
    #[token("|=")]
    PipeEq,
    #[token("^=")]
    CaretEq,
    #[token("<<=")]
    ShlEq,
    #[token(">>=")]
    ShrEq,
    #[token("&^=")]
    AmpCaretEq,
    #[token("&&")]
    AmpAmp,
    #[token("||")]
    PipePipe,
    #[token("<-")]
    Arrow,
    #[token("++")]
    PlusPlus,
    #[token("--")]
    MinusMinus,
    #[token("==")]
    EqEq,
    #[token("<")]
    Lt,
    #[token(">")]
    Gt,
    #[token("=")]
    Eq,
    #[token("!")]
    Bang,
    #[token("~")]
    Tilde,
    #[token("!=")]
    NotEq,
    #[token("<=")]
    LtEq,
    #[token(">=")]
    GtEq,
    #[token(":=")]
    ColonEq,
    #[token("...")]
    Ellipsis,
    #[token("(")]
    LParen,
    #[token("[")]
    LBrack,
    #[token("{")]
    LBrace,
    #[token(",")]
    Comma,
    #[token(".")]
    Dot,
    #[token(")")]
    RParen,
    #[token("]")]
    RBrack,
    #[token("}")]
    RBrace,
    /// Written `;`, or inserted at a line end, in which case the token's
    /// text is not `;`.
    #[token(";")]
    #[token("\n", lexer::newline)]
    #[regex("//[^\n]*", lexer::line_comment, allow_greedy = true)]
    #[token("/*", lexer::block_comment)]
    #[regex(r"[\x00-\x08\x0B\x0C\x0E-\x1F$?\\\x7F]", lexer::illegal)]
    Semi,
    #[token(":")]
    Colon,
    #[token("#", lexer::gopp_only)]
    Hash,
    #[token("@", lexer::gopp_only)]
    At,

    Break,
    Case,
    Chan,
    Const,
    Continue,
    Default,
    Defer,
    Else,
    Fallthrough,
    For,
    Func,
    Go,
    Goto,
    If,
    Import,
    Interface,
    Macro,
    Map,
    Package,
    Range,
    Return,
    Select,
    Struct,
    Switch,
    Type,
    Var,

    Eof,
    /// What the lexer leaves out of the token list: a comment, a line end
    /// that is no `;`, an illegal character.
    Skipped,
}

impl TokenKind {
    /// The keyword spelled `word`, if any. `macro` is one only in Go++ files.
    pub(crate) fn keyword(word: &str) -> Option<Self> {
        Some(match word {
            "break" => Self::Break,
            "case" => Self::Case,
            "chan" => Self::Chan,
            "const" => Self::Const,
            "continue" => Self::Continue,
            "default" => Self::Default,
            "defer" => Self::Defer,
            "else" => Self::Else,
            "fallthrough" => Self::Fallthrough,
            "for" => Self::For,
            "func" => Self::Func,
            "go" => Self::Go,
            "goto" => Self::Goto,
            "if" => Self::If,
            "import" => Self::Import,
            "interface" => Self::Interface,
            "macro" => Self::Macro,
            "map" => Self::Map,
            "package" => Self::Package,
            "range" => Self::Range,
            "return" => Self::Return,
            "select" => Self::Select,
            "struct" => Self::Struct,
            "switch" => Self::Switch,
            "type" => Self::Type,
            "var" => Self::Var,
            _ => return None,
        })
    }

    /// Whether a line end after this token inserts a semicolon (Go's rule).
    pub(crate) fn ends_statement(self) -> bool {
        matches!(
            self,
            Self::Ident
                | Self::Int
                | Self::Float
                | Self::Imag
                | Self::Rune
                | Self::String
                | Self::Break
                | Self::Continue
                | Self::Fallthrough
                | Self::Return
                | Self::PlusPlus
                | Self::MinusMinus
                | Self::RParen
                | Self::RBrack
                | Self::RBrace
        )
    }
}
