use std::fs;
use std::path::Path;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{SourceFile, SourceMap};
use proptest::prelude::*;

use super::{Lexed, lex};
use crate::token::TokenKind::{self, *};

/// `String` here is the token kind.
type Text = std::string::String;

fn lex_bytes(bytes: &[u8]) -> (SourceFile, Lexed, Vec<Diagnostic>) {
    let source = SourceFile::from_bytes("test.gopp".into(), bytes).unwrap();
    let map = SourceMap::new();
    let file = map.add(SourceFile::new("test.gopp".into(), Text::new()).unwrap());
    let sink = Sink::new(1000);
    let lexed = lex(file, &source, &sink);
    (source, lexed, sink.finish())
}

/// Kinds and texts of the tokens of `text`, without the final `Eof`.
fn tokens_in(text: &str) -> Vec<(TokenKind, Text)> {
    let (source, lexed, diagnostics) = lex_bytes(text.as_bytes());
    assert_eq!(diagnostics, [], "{text:?}");
    let (eof, tokens) = lexed.tokens.split_last().unwrap();
    assert_eq!(eof.kind, Eof);
    tokens
        .iter()
        .map(|token| {
            assert!(!token.bad);
            let text = &source.text()[token.lo as usize..token.hi as usize];
            (token.kind, text.to_owned())
        })
        .collect()
}

fn kinds(text: &str) -> Vec<TokenKind> {
    let tokens = tokens_in(text);
    tokens.into_iter().map(|(kind, _)| kind).collect()
}

/// The one diagnostic of `text`: its code, start offset and message.
fn error_in(text: &str) -> (Code, u32, Text) {
    let (_, _, diagnostics) = lex_bytes(text.as_bytes());
    assert_eq!(diagnostics.len(), 1, "{text:?}: {diagnostics:?}");
    let diagnostic = &diagnostics[0];
    (
        diagnostic.code,
        diagnostic.span.lo,
        diagnostic.message.clone(),
    )
}

#[test]
fn every_operator_and_delimiter_is_one_token() {
    let table = [
        ("+", Plus),
        ("-", Minus),
        ("*", Star),
        ("/", Slash),
        ("%", Percent),
        ("&", Amp),
        ("|", Pipe),
        ("^", Caret),
        ("<<", Shl),
        (">>", Shr),
        ("&^", AmpCaret),
        ("+=", PlusEq),
        ("-=", MinusEq),
        ("*=", StarEq),
        ("/=", SlashEq),
        ("%=", PercentEq),
        ("&=", AmpEq),
        ("|=", PipeEq),
        ("^=", CaretEq),
        ("<<=", ShlEq),
        (">>=", ShrEq),
        ("&^=", AmpCaretEq),
        ("&&", AmpAmp),
        ("||", PipePipe),
        ("<-", Arrow),
        ("++", PlusPlus),
        ("--", MinusMinus),
        ("==", EqEq),
        ("<", Lt),
        (">", Gt),
        ("=", Eq),
        ("!", Bang),
        ("~", Tilde),
        ("!=", NotEq),
        ("<=", LtEq),
        (">=", GtEq),
        (":=", ColonEq),
        ("...", Ellipsis),
        ("(", LParen),
        ("[", LBrack),
        ("{", LBrace),
        (",", Comma),
        (".", Dot),
        (")", RParen),
        ("]", RBrack),
        ("}", RBrace),
        (";", Semi),
        (":", Colon),
        ("#", Hash),
        ("@", At),
    ];
    for (text, kind) in table {
        let tokens = tokens_in(&format!("{text} x"));
        assert_eq!(tokens[0], (kind, text.to_owned()));
        assert_eq!(tokens[1].0, Ident, "{text}");
    }
}

#[test]
fn every_keyword_is_its_own_kind() {
    let table = [
        ("break", Break),
        ("case", Case),
        ("chan", Chan),
        ("const", Const),
        ("continue", Continue),
        ("default", Default),
        ("defer", Defer),
        ("else", Else),
        ("fallthrough", Fallthrough),
        ("for", For),
        ("func", Func),
        ("go", Go),
        ("goto", Goto),
        ("if", If),
        ("import", Import),
        ("interface", Interface),
        ("macro", Macro),
        ("map", Map),
        ("package", Package),
        ("range", Range),
        ("return", Return),
        ("select", Select),
        ("struct", Struct),
        ("switch", Switch),
        ("type", Type),
        ("var", Var),
    ];
    for (text, kind) in table {
        assert_eq!(kinds(&format!("{text} {{")), [kind, LBrace]);
        assert_eq!(
            kinds(&format!("{text}s x{text} {text}é _{text} {{"))[..4],
            [Ident; 4]
        );
    }
}

#[test]
fn macro_is_a_keyword() {
    assert_eq!(tokens_in("macro F")[0].0, Macro);
}

#[test]
fn contextual_words_are_identifiers() {
    assert_eq!(kinds("extends base {"), [Ident, Ident, LBrace]);
}

#[test]
fn lexing_is_greedy() {
    assert_eq!(kinds("a<<=b"), [Ident, ShlEq, Ident, Semi]);
    assert_eq!(
        kinds("a&^=b&^c"),
        [Ident, AmpCaretEq, Ident, AmpCaret, Ident, Semi]
    );
    assert_eq!(kinds("a<-b"), [Ident, Arrow, Ident, Semi]);
    assert_eq!(kinds("x...."), [Ident, Ellipsis, Dot]);
    assert_eq!(kinds("a.b"), [Ident, Dot, Ident, Semi]);
}

#[test]
fn bang_joins_only_an_equals_sign() {
    assert_eq!(kinds("f!(x)"), [Ident, Bang, LParen, Ident, RParen, Semi]);
    assert_eq!(
        kinds("f != (x)"),
        [Ident, NotEq, LParen, Ident, RParen, Semi]
    );
    assert_eq!(kinds("f[T]!(x)")[..5], [Ident, LBrack, Ident, RBrack, Bang]);
    assert_eq!(kinds("func!(x int) T")[..3], [Func, Bang, LParen]);
    assert_eq!(kinds("!x"), [Bang, Ident, Semi]);
    assert_eq!(kinds("!!=x"), [Bang, NotEq, Ident, Semi]);
}

#[test]
fn a_line_end_inserts_a_semicolon_after_go_s_tokens() {
    for text in [
        "x",
        "1",
        "1.5",
        "2i",
        "'a'",
        "\"s\"",
        "`s`",
        "break",
        "continue",
        "fallthrough",
        "return",
        "x++",
        "x--",
        "f()",
        "a[0]",
        "T{}",
    ] {
        let tokens = tokens_in(&format!("{text}\ny"));
        let semi = &tokens[tokens.len() - 3];
        assert_eq!(*semi, (Semi, "\n".to_owned()), "{text}");
    }
    for text in [
        "+", "(", "[", "{", ",", ".", "=", ":", "func", "if", "!", ";", "#", "@",
    ] {
        assert!(
            !kinds(&format!("x {text}\ny"))[2..].starts_with(&[Semi]),
            "{text}"
        );
        assert_eq!(kinds(&format!("{text}\n")).len(), 1, "{text}");
    }
}

#[test]
fn a_line_ending_in_bang_continues_on_the_next() {
    assert_eq!(
        kinds("f!\n(x)\n"),
        [Ident, Bang, LParen, Ident, RParen, Semi]
    );
}

#[test]
fn an_attribute_line_ends_with_a_semicolon() {
    assert_eq!(kinds("@Pinned\ntype"), [At, Ident, Semi, Type]);
    assert_eq!(
        kinds("@Derive(JSON)\ntype"),
        [At, Ident, LParen, Ident, RParen, Semi, Type]
    );
}

#[test]
fn the_end_of_the_file_ends_the_last_line() {
    assert_eq!(tokens_in("x"), [(Ident, "x".into()), (Semi, "".into())]);
    assert_eq!(kinds("x // c"), [Ident, Semi]);
    assert_eq!(kinds("x +"), [Ident, Plus]);
    assert_eq!(kinds(""), []);
    assert_eq!(kinds("\n\n"), []);
}

#[test]
fn comments_end_a_line_only_with_a_line_end_in_them() {
    assert_eq!(kinds("x /* c */ y"), [Ident, Ident, Semi]);
    assert_eq!(kinds("x /* a\nb */ y"), [Ident, Semi, Ident, Semi]);
    assert_eq!(kinds("x // c\ny"), [Ident, Semi, Ident, Semi]);
    assert_eq!(kinds("( /* a\nb */ y"), [LParen, Ident, Semi]);
    assert_eq!(kinds("x /* a\nb */\n y"), [Ident, Semi, Ident, Semi]);
    let tokens = tokens_in("x /* a\nb */ y");
    assert_eq!(tokens[1], (Semi, Text::new()));
}

#[test]
fn comments_are_kept_in_order() {
    let text = "// first\nx /* block */ y //gopp:x\n/* // no */";
    let (source, lexed, diagnostics) = lex_bytes(text.as_bytes());
    assert_eq!(diagnostics, []);
    let comments: Vec<_> = lexed
        .comments
        .iter()
        .map(|c| &source.text()[c.lo as usize..c.hi as usize])
        .collect();
    assert_eq!(
        comments,
        ["// first", "/* block */", "//gopp:x", "/* // no */"]
    );
}

#[test]
fn a_build_comment_above_the_package_clause_is_an_error() {
    let messages = |text: &str| {
        let (_, _, diagnostics) = lex_bytes(text.as_bytes());
        let message = |d: &Diagnostic| {
            assert_eq!(d.code, Code::BadBuildConstraint);
            (d.span.lo, d.message.clone())
        };
        diagnostics.iter().map(message).collect::<Vec<_>>()
    };
    let told = |condition: &str| {
        format!("the build condition of a .gopp file is an attribute: write `@Build({condition})`")
    };
    assert_eq!(
        messages("// c\n//go:build linux && !arm64\n\npackage p\n"),
        [(5, told("linux && !arm64"))]
    );
    for fine in [
        "package p\n//go:build linux\n",
        "// go:build linux\n//go:builder\n/* //go:build linux */\npackage p\n",
    ] {
        assert_eq!(messages(fine), [], "{fine}");
    }
}

#[test]
fn compiler_flag_directives_belong_in_gopp_mod() {
    for name in ["cflags", "cxxstd", "ldflags"] {
        let text = format!("package p\n//gopp:{name} -O2 -DX  \n");
        let (_, _, diagnostics) = lex_bytes(text.as_bytes());
        assert_eq!(diagnostics.len(), 1);
        let diagnostic = &diagnostics[0];
        assert_eq!(diagnostic.code, Code::GoppDirectiveInSource);
        assert_eq!(diagnostic.span.lo, 10);
        assert_eq!(
            diagnostic.message,
            format!("//gopp:{name} is not a directive of source files")
        );
        assert_eq!(diagnostic.notes[0].code, Code::NoteGoppModLine);
        assert_eq!(
            diagnostic.notes[0].message,
            format!("add the line `{name} -O2 -DX` to gopp.mod")
        );
    }
    assert_eq!(kinds("//gopp:cflagsx\n// gopp:cflags\n"), []);
}

#[test]
fn identifiers_follow_unicode() {
    for text in ["x", "_", "_x9", "αβγ", "日本語", "x١", "café", "a\u{0301}"] {
        assert_eq!(tokens_in(text)[0], (Ident, text.to_owned()), "{text}");
    }
    assert_eq!(kinds("x.y"), [Ident, Dot, Ident, Semi]);
}

#[test]
fn well_formed_numbers() {
    let table = [
        ("0", Int),
        ("123", Int),
        ("0b0", Int),
        ("0b1010", Int),
        ("0B110", Int),
        ("0o0", Int),
        ("0o1234567", Int),
        ("0O17", Int),
        ("0x0", Int),
        ("0xcafeF00D", Int),
        ("0XCAFE", Int),
        ("01234567", Int),
        ("0_1", Int),
        ("1_000_000", Int),
        ("0x_1", Int),
        ("0b_1", Int),
        ("0o_7", Int),
        ("0.", Float),
        ("123.", Float),
        ("0123.", Float),
        (".0", Float),
        (".123", Float),
        ("1.5", Float),
        ("0e0", Float),
        ("1e+10", Float),
        ("1E-2", Float),
        ("089.5", Float),
        ("09e1", Float),
        ("1_0.2_5e1_0", Float),
        ("0x1p0", Float),
        ("0X1P-10", Float),
        ("0x1.fp3", Float),
        ("0x.8p1", Float),
        ("0x_1.p+2", Float),
        ("0i", Imag),
        ("123i", Imag),
        ("0123i", Imag),
        ("09i", Imag),
        ("0x10i", Imag),
        ("0b11i", Imag),
        ("1.5i", Imag),
        (".5i", Imag),
        ("1e3i", Imag),
        ("0x1p-2i", Imag),
    ];
    for (text, kind) in table {
        assert_eq!(
            tokens_in(text),
            [(kind, text.to_owned()), (Semi, Text::new())]
        );
    }
}

#[test]
fn a_number_ends_where_go_s_does() {
    assert_eq!(kinds("1..2"), [Float, Float, Semi]);
    assert_eq!(kinds("1.x"), [Float, Ident, Semi]);
    assert_eq!(kinds("0x1e+1"), [Int, Plus, Int, Semi]);
    assert_eq!(kinds("1if"), [Imag, Ident, Semi]);
    assert_eq!(kinds("x.5"), [Ident, Float, Semi]);
    assert_eq!(kinds("a[1:2]")[2..5], [Int, Colon, Int]);
}

#[test]
fn malformed_numbers() {
    let table = [
        (
            "0b1.0",
            Code::LexRadixPoint,
            3,
            "invalid radix point in binary literal",
        ),
        (
            "0o1.5",
            Code::LexRadixPoint,
            3,
            "invalid radix point in octal literal",
        ),
        ("0b", Code::LexNoDigits, 0, "binary literal has no digits"),
        ("0o", Code::LexNoDigits, 0, "octal literal has no digits"),
        (
            "0x",
            Code::LexNoDigits,
            0,
            "hexadecimal literal has no digits",
        ),
        (
            "0x_",
            Code::LexNoDigits,
            0,
            "hexadecimal literal has no digits",
        ),
        (
            "0b1e1",
            Code::LexExponentNeedsDecimal,
            3,
            "'e' exponent requires decimal mantissa",
        ),
        (
            "0o7E2",
            Code::LexExponentNeedsDecimal,
            3,
            "'E' exponent requires decimal mantissa",
        ),
        (
            "1p1",
            Code::LexExponentNeedsHex,
            1,
            "'p' exponent requires hexadecimal mantissa",
        ),
        (
            "0b1P1",
            Code::LexExponentNeedsHex,
            3,
            "'P' exponent requires hexadecimal mantissa",
        ),
        ("1e", Code::LexExponentNoDigits, 2, "exponent has no digits"),
        (
            "1e+",
            Code::LexExponentNoDigits,
            3,
            "exponent has no digits",
        ),
        (
            "0x1p",
            Code::LexExponentNoDigits,
            4,
            "exponent has no digits",
        ),
        (
            "0x1.0",
            Code::LexHexMantissaNeedsP,
            0,
            "hexadecimal mantissa requires a 'p' exponent",
        ),
        (
            "0x.8",
            Code::LexHexMantissaNeedsP,
            0,
            "hexadecimal mantissa requires a 'p' exponent",
        ),
        (
            "0b12",
            Code::LexInvalidDigit,
            3,
            "invalid digit '2' in binary literal",
        ),
        (
            "0o18",
            Code::LexInvalidDigit,
            3,
            "invalid digit '8' in octal literal",
        ),
        (
            "089",
            Code::LexInvalidDigit,
            1,
            "invalid digit '8' in octal literal",
        ),
        (
            "1__0",
            Code::LexBadSeparator,
            2,
            "'_' must separate successive digits",
        ),
        (
            "1_",
            Code::LexBadSeparator,
            1,
            "'_' must separate successive digits",
        ),
        (
            "0x1_",
            Code::LexBadSeparator,
            3,
            "'_' must separate successive digits",
        ),
        (
            "0_x1",
            Code::LexBadSeparator,
            1,
            "'_' must separate successive digits",
        ),
        (
            "1_.5",
            Code::LexBadSeparator,
            1,
            "'_' must separate successive digits",
        ),
        (
            "1._5",
            Code::LexBadSeparator,
            2,
            "'_' must separate successive digits",
        ),
        (
            "1e_5",
            Code::LexBadSeparator,
            2,
            "'_' must separate successive digits",
        ),
        (
            "1_i",
            Code::LexBadSeparator,
            1,
            "'_' must separate successive digits",
        ),
    ];
    for (text, code, offset, message) in table {
        assert_eq!(error_in(text), (code, offset, message.to_owned()), "{text}");
        let (_, lexed, _) = lex_bytes(text.as_bytes());
        assert!(lexed.tokens[0].bad, "{text}");
    }
}

#[test]
fn well_formed_runes_and_strings() {
    let table = [
        ("'a'", Rune),
        ("'é'", Rune),
        ("'\\n'", Rune),
        ("'\\''", Rune),
        ("'\\\\'", Rune),
        ("'\\x41'", Rune),
        ("'\\101'", Rune),
        ("'\\u00e9'", Rune),
        ("'\\U0001F600'", Rune),
        ("'\"'", Rune),
        ("'\\377'", Rune),
        ("'\\U0010FFFF'", Rune),
        ("\"\"", String),
        ("\"a\\tb\"", String),
        ("\"\\\"\"", String),
        ("\"'\"", String),
        ("\"\\a\\b\\f\\n\\r\\t\\v\\\\\"", String),
        ("\"日本\\u65e5\"", String),
        ("``", String),
        ("`a\nb\\`", String),
        ("`\"'`", String),
        ("`\r\n`", String),
        ("\"// no comment\"", String),
        ("\"/* no comment\"", String),
    ];
    for (text, kind) in table {
        assert_eq!(
            tokens_in(text),
            [(kind, text.to_owned()), (Semi, Text::new())]
        );
    }
}

#[test]
fn malformed_runes_and_strings() {
    let table = [
        (
            "''",
            Code::LexRuneIllegal,
            1,
            "empty rune literal or unescaped ' in rune literal",
        ),
        (
            "'ab'",
            Code::LexRuneIllegal,
            0,
            "more than one character in rune literal",
        ),
        (
            "'a",
            Code::LexRuneUnterminated,
            0,
            "rune literal not terminated",
        ),
        (
            "'a\n",
            Code::LexRuneUnterminated,
            2,
            "newline in rune literal",
        ),
        (
            "'\\",
            Code::LexRuneUnterminated,
            0,
            "rune literal not terminated",
        ),
        ("'\\q'", Code::LexEscapeUnknown, 2, "unknown escape"),
        ("'\\\"'", Code::LexEscapeUnknown, 2, "unknown escape"),
        ("\"\\'\"", Code::LexEscapeUnknown, 2, "unknown escape"),
        (
            "'\\xZ1'",
            Code::LexEscapeIllegalChar,
            3,
            "invalid character 'Z' in hexadecimal escape",
        ),
        (
            "'\\08'",
            Code::LexEscapeIllegalChar,
            3,
            "invalid character '8' in octal escape",
        ),
        (
            "\"\\u12\"",
            Code::LexEscapeIllegalChar,
            5,
            "invalid character '\"' in hexadecimal escape",
        ),
        (
            "'\\400'",
            Code::LexEscapeBadCodePoint,
            2,
            "octal escape value 256 > 255",
        ),
        (
            "'\\ud800'",
            Code::LexEscapeBadCodePoint,
            3,
            "escape is invalid Unicode code point U+D800",
        ),
        (
            "'\\U00110000'",
            Code::LexEscapeBadCodePoint,
            3,
            "escape is invalid Unicode code point U+110000",
        ),
        (
            "\"abc",
            Code::LexStringUnterminated,
            0,
            "string not terminated",
        ),
        (
            "\"abc\n",
            Code::LexStringUnterminated,
            4,
            "newline in string",
        ),
        (
            "\"\\",
            Code::LexStringUnterminated,
            0,
            "string not terminated",
        ),
        (
            "`abc\n",
            Code::LexRawStringUnterminated,
            0,
            "string not terminated",
        ),
    ];
    for (text, code, offset, message) in table {
        assert_eq!(
            error_in(text),
            (code, offset, message.to_owned()),
            "{text:?}"
        );
        let (_, lexed, _) = lex_bytes(text.as_bytes());
        assert!(lexed.tokens[0].bad, "{text:?}");
    }
}

#[test]
fn lexing_goes_on_after_a_bad_literal() {
    let (_, lexed, diagnostics) = lex_bytes(b"x := \"abc\ny := 0b12\nz");
    let kinds: Vec<_> = lexed.tokens.iter().map(|t| t.kind).collect();
    assert_eq!(
        kinds,
        [
            Ident, ColonEq, String, Semi, Ident, ColonEq, Int, Semi, Ident, Semi, Eof
        ]
    );
    let bad: Vec<_> = lexed.tokens.iter().map(|t| t.bad).collect();
    assert_eq!(bad.iter().filter(|&&bad| bad).count(), 2);
    assert!(bad[2] && bad[6]);
    assert_eq!(diagnostics.len(), 2);
}

#[test]
fn an_unterminated_comment_is_reported_at_its_start() {
    assert_eq!(
        error_in("x /* never closed\n"),
        (
            Code::LexCommentUnterminated,
            2,
            "comment not terminated".into()
        )
    );
    assert_eq!(error_in("/*/").0, Code::LexCommentUnterminated);
    assert_eq!(kinds("/**/ /***/"), []);
}

#[test]
fn characters_no_token_starts_with() {
    let table = [
        ("$", "invalid character U+0024 '$'"),
        ("?", "invalid character U+003F '?'"),
        ("\\", "invalid character U+005C '\\'"),
        ("\u{7}", "invalid character U+0007"),
        ("€", "invalid character U+20AC '€'"),
        ("\u{FFFD}", "invalid character U+FFFD '\u{FFFD}'"),
        ("٣", "invalid character U+0663 '٣'"),
    ];
    for (text, message) in table {
        let source = format!("x {text} y");
        assert_eq!(
            error_in(&source),
            (Code::LexIllegalChar, 2, message.to_owned())
        );
        let (_, lexed, _) = lex_bytes(source.as_bytes());
        assert_eq!(lexed.tokens.len(), 4, "{text:?}");
    }
}

#[test]
fn curly_quotes_name_the_neutral_one() {
    assert_eq!(
        error_in("“"),
        (
            Code::LexCurlyQuote,
            0,
            "curly quotation mark '“' (use neutral '\"')".into()
        )
    );
    assert_eq!(
        error_in("x = ’"),
        (
            Code::LexCurlyQuote,
            4,
            "curly quotation mark '’' (use neutral '\\'')".into()
        )
    );
    assert_eq!(kinds("\"“”\" // ‘’"), [String, Semi]);
}

#[test]
fn nul_is_an_error_everywhere() {
    assert_eq!(
        error_in("x \0 y"),
        (Code::LexIllegalNul, 2, "invalid NUL character".into())
    );
    assert_eq!(error_in("\"a\0b\"").0, Code::LexIllegalNul);
    assert_eq!(error_in("// a\0b").0, Code::LexIllegalNul);
}

#[test]
fn a_byte_order_mark_is_legal_only_at_the_start() {
    assert_eq!(kinds("\u{FEFF}package p"), [Package, Ident, Semi]);
    assert_eq!(
        error_in("package\u{FEFF} p"),
        (
            Code::LexIllegalBom,
            7,
            "invalid BOM in the middle of the file".into()
        )
    );
    assert_eq!(error_in("\"\u{FEFF}\"").0, Code::LexIllegalBom);
}

#[test]
fn invalid_utf8_is_reported_once_per_sequence() {
    let (_, lexed, diagnostics) = lex_bytes(b"x \xFF y \"\xC3\" // \xE2\x82\n");
    let found: Vec<_> = diagnostics.iter().map(|d| (d.code, d.span.lo)).collect();
    assert_eq!(
        found,
        [
            (Code::LexIllegalUtf8, 2),
            (Code::LexIllegalUtf8, 9),
            (Code::LexIllegalUtf8, 17)
        ]
    );
    assert_eq!(diagnostics[0].message, "invalid UTF-8 encoding");
    let kinds: Vec<_> = lexed.tokens.iter().map(|t| t.kind).collect();
    assert_eq!(kinds, [Ident, Ident, String, Semi, Eof]);
}

#[test]
fn a_utf16_file_is_one_error() {
    let (_, lexed, diagnostics) = lex_bytes(b"\xFF\xFEp\0a\0c\0k\0");
    assert_eq!(diagnostics.len(), 1);
    assert_eq!(diagnostics[0].code, Code::LexUtf16);
    assert_eq!(
        diagnostics[0].message,
        "file is UTF-16; source files must be UTF-8"
    );
    assert_eq!(lexed.tokens.len(), 1);
}

#[test]
fn the_spec_s_lexical_example() {
    let text = "func load!(path string) []byte {\n\tf := os.Open!(path)\n\treturn io.ReadAll!(f)\n}\n\n@Pinned\ntype Gate struct{ n int }\n\nfunc check(x int) {\n\t#Assert(x > 0, \"x must be positive\")\n\tif x != 3 && !(x < 0) { }\n}\n";
    let kinds = kinds(text);
    assert_eq!(kinds[..4], [Func, Ident, Bang, LParen]);
    assert!(kinds.windows(3).any(|w| w == [At, Ident, Semi]));
    assert!(kinds.windows(3).any(|w| w == [Hash, Ident, LParen]));
    assert!(kinds.windows(4).any(|w| w == [NotEq, Int, AmpAmp, Bang]));
}

#[test]
fn every_sample_lexes_without_a_diagnostic() {
    let examples = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../docs/spec/examples");
    let mut checked = 0;
    for entry in fs::read_dir(examples).unwrap() {
        let dir = entry.unwrap().path();
        if !dir.is_dir() {
            continue;
        }
        for file in fs::read_dir(dir).unwrap() {
            let path = file.unwrap().path();
            if !gopp_span::is_source_path(&path) {
                continue;
            }
            let (_, lexed, diagnostics) = lex_bytes(&fs::read(&path).unwrap());
            assert_eq!(diagnostics, [], "{}", path.display());
            assert!(lexed.tokens.len() > 10, "{}", path.display());
            checked += 1;
        }
    }
    assert!(checked >= 10, "only {checked} sample files found");
}

/// Bytes that reach every scanner: quotes, escapes, comment marks, digits,
/// line ends, bad UTF-8.
fn source_bytes() -> impl Strategy<Value = Vec<u8>> {
    let piece = prop_oneof![
        Just(b"\"".to_vec()),
        Just(b"'".to_vec()),
        Just(b"`".to_vec()),
        Just(b"\\".to_vec()),
        Just(b"/*".to_vec()),
        Just(b"*/".to_vec()),
        Just(b"//".to_vec()),
        Just(b"\n".to_vec()),
        Just(b"0x".to_vec()),
        Just(b"1_".to_vec()),
        Just(b".5e".to_vec()),
        Just(b"\0".to_vec()),
        Just(b"\xFF".to_vec()),
        Just("\u{FEFF}".as_bytes().to_vec()),
        Just(b"!".to_vec()),
        Just("é".as_bytes().to_vec()),
        Just(b"x".to_vec()),
        Just(b" ".to_vec()),
        proptest::collection::vec(any::<u8>(), 0..4),
    ];
    proptest::collection::vec(piece, 0..40).prop_map(|pieces| pieces.concat())
}

proptest! {
    #[test]
    fn any_bytes_lex_into_ordered_tokens_that_cover_the_text(bytes in source_bytes()) {
        let (source, lexed, diagnostics) = lex_bytes(&bytes);
        let text = source.text();
        prop_assume!(!source.is_utf16());

        let (eof, tokens) = lexed.tokens.split_last().unwrap();
        prop_assert_eq!((eof.kind, eof.lo as usize), (Eof, text.len()));
        let mut pieces: Vec<(u32, u32)> = tokens.iter().map(|t| (t.lo, t.hi)).collect();
        pieces.extend(lexed.comments.iter().map(|c| (c.lo, c.hi)));
        pieces.sort();

        let mut covered = 0;
        for (lo, hi) in pieces {
            prop_assert!(covered <= lo && lo <= hi && hi as usize <= text.len());
            prop_assert!(text.is_char_boundary(lo as usize) && text.is_char_boundary(hi as usize));
            let gap = &text[covered as usize..lo as usize];
            for (offset, c) in gap.char_indices() {
                let at = covered + offset as u32;
                let reported = diagnostics.iter().any(|d| d.span.lo <= at && at < d.span.hi);
                let leading_bom = at == 0 && c == '\u{FEFF}';
                prop_assert!(c.is_ascii_whitespace() || reported || leading_bom, "{c:?} at {at}");
            }
            covered = hi;
        }
    }
}

/// What the lexer leaves out costs no stack, however much of it comes in
/// a row.
#[test]
fn a_long_run_of_skipped_text_is_lexed_in_a_loop() {
    let count = 200_000;
    let skipped = [
        "\n".repeat(count),
        "// c\n".repeat(count),
        "/* c */ ".repeat(count),
    ];
    for run in skipped {
        let (_, lexed, diagnostics) = lex_bytes(format!("x{run}y").as_bytes());
        assert_eq!(diagnostics, []);
        let kinds: Vec<TokenKind> = lexed.tokens.iter().map(|token| token.kind).collect();
        assert!(
            matches!(kinds[..], [Ident, .., Ident, Semi, Eof]),
            "{kinds:?}"
        );
    }
    let (_, lexed, diagnostics) = lex_bytes("$".repeat(count).as_bytes());
    assert_eq!(lexed.tokens.len(), 1);
    assert_eq!(diagnostics.len(), 1001);
}
