//! The stable codes of the diagnostics.

use std::fmt;

use crate::Severity;

macro_rules! codes {
    ($($number:literal $variant:ident $name:literal,)*) => {
        /// A diagnostic's identity. Number and name are the row in
        /// `docs/errors/codes.lock`, which never changes once written.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
        #[repr(u16)]
        pub enum Code {
            $($variant = $number,)*
        }

        impl Code {
            pub const ALL: &[Code] = &[$(Code::$variant,)*];

            pub fn name(self) -> &'static str {
                match self {
                    $(Code::$variant => $name,)*
                }
            }
        }
    };
}

codes! {
    1 TooManyErrors "err_too_many_errors",
    2 LexIllegalNul "err_lex_illegal_nul",
    3 LexUtf16 "err_lex_utf16",
    4 LexIllegalUtf8 "err_lex_illegal_utf8",
    5 LexIllegalBom "err_lex_illegal_bom",
    6 LexIllegalChar "err_lex_illegal_char",
    7 LexCurlyQuote "err_lex_curly_quote",
    8 LexCommentUnterminated "err_lex_comment_unterminated",
    9 LexRadixPoint "err_lex_radix_point",
    10 LexNoDigits "err_lex_no_digits",
    11 LexExponentNeedsDecimal "err_lex_exponent_needs_decimal",
    12 LexExponentNeedsHex "err_lex_exponent_needs_hex",
    13 LexExponentNoDigits "err_lex_exponent_no_digits",
    14 LexHexMantissaNeedsP "err_lex_hex_mantissa_needs_p",
    15 LexInvalidDigit "err_lex_invalid_digit",
    16 LexBadSeparator "err_lex_bad_separator",
    18 LexEscapeUnknown "err_lex_escape_unknown",
    19 LexEscapeIllegalChar "err_lex_escape_illegal_char",
    20 LexEscapeBadCodePoint "err_lex_escape_bad_code_point",
    21 LexRuneUnterminated "err_lex_rune_unterminated",
    22 LexRuneIllegal "err_lex_rune_illegal",
    23 LexStringUnterminated "err_lex_string_unterminated",
    24 LexRawStringUnterminated "err_lex_raw_string_unterminated",
    25 Expected "err_expected",
    26 ExpectedFound "err_expected_found",
    28 MissingComma "err_missing_comma",
    29 MissingCommaNewline "err_missing_comma_newline",
    30 NestingTooDeep "err_nesting_too_deep",
    31 ExpressionTooDeeplyNested "err_expression_too_deeply_nested",
    32 UnexpectedCommaRbrack "err_unexpected_comma_rbrack",
    33 ParenEmbeddedType "err_paren_embedded_type",
    34 ParenComplitType "err_paren_complit_type",
    35 MissingTypeConstraint "err_missing_type_constraint",
    36 MissingTypeParamName "err_missing_type_param_name",
    37 MissingTypeParamNameOrLen "err_missing_type_param_name_or_len",
    38 MissingParamType "err_missing_param_type",
    39 MissingParamName "err_missing_param_name",
    41 DotsInvalid "err_dots_invalid",
    42 EmptyTypeParams "err_empty_type_params",
    43 FuncTypeTypeParams "err_func_type_type_params",
    44 MethodTypeParams "err_method_type_params",
    45 InterfaceMethodTypeParams "err_interface_method_type_params",
    46 Slice3MiddleIndex "err_slice3_middle_index",
    47 Slice3FinalIndex "err_slice3_final_index",
    49 CallParenthesized "err_call_parenthesized",
    51 MissingIfCondition "err_missing_if_condition",
    52 VarInIfInit "err_var_in_if_init",
    53 NewlineAfterIfClause "err_newline_after_if_clause",
    54 NewlineBeforeLbrace "err_newline_before_lbrace",
    55 ImportPathNotString "err_import_path_not_string",
    56 MissingImportPath "err_missing_import_path",
    57 ImportAfterDecl "err_import_after_decl",
    58 RangeAliasAssign "err_range_alias_assign",
    59 RangeAliasKey "err_range_alias_key",
    64 MacroNameLowercase "err_macro_name_lowercase",
    65 UnknownMacroKind "err_unknown_macro_kind",
    66 AttrOnStmt "err_attr_on_stmt",
    67 AttrOnImport "err_attr_on_import",
    72 GoppDirectiveInSource "err_gopp_directive_in_source",
    73 NoteGoppModLine "note_gopp_mod_line",
    79 StmtOutsideFunc "err_stmt_outside_func",
    82 ExpectedLbraceAfter "err_expected_lbrace_after",
    108 BadBuildConstraint "err_load_bad_build_constraint",
    109 BadImportPath "err_load_bad_import_path",
    111 Undefined "err_undefined",
    112 Redeclared "err_redeclared",
    113 NoteOtherDeclaration "note_other_declaration",
    114 DeclaredNotUsed "err_declared_not_used",
    127 UnexportedName "err_unexported_name",
    129 CannotUseBlank "err_cannot_use_blank",
    131 InitSignature "err_init_signature",
    132 InitWithBody "err_init_with_body",
    133 InitTypeParams "err_init_type_params",
    136 ImportUnresolved "err_import_unresolved",
    137 NotAType "err_not_a_type",
    138 NotAnExpression "err_not_an_expression",
    139 InvalidRecursiveType "err_invalid_recursive_type",
    140 InvalidRecursiveTypeSelf "err_invalid_recursive_type_self",
    141 InvalidCycle "err_invalid_cycle",
    142 InitCycle "err_init_cycle",
    143 InitCycleSelf "err_init_cycle_self",
    144 InvalidArrayLength "err_invalid_array_length",
    156 ReceiverCount "err_receiver_count",
    157 ReceiverMissing "err_receiver_missing",
    160 CannotConvert "err_cannot_convert",
    163 ValueTruncated "err_value_truncated",
    164 ValueOverflows "err_value_overflows",
    166 OperatorNotDefined "err_operator_not_defined",
    167 MismatchedTypes "err_mismatched_types",
    168 CannotCompare "err_cannot_compare",
    169 DivisionByZero "err_division_by_zero",
    170 ShiftCountNegative "err_shift_count_negative",
    171 ShiftOperandInvalid "err_shift_operand_invalid",
    172 ShiftCountInvalid "err_shift_too_large",
    173 ConstantExpressionOverflow "err_constant_expression_overflow",
    175 NoValue "err_no_value",
    176 MultiValueInSingle "err_multi_value_in_single",
    177 BuiltinNotCalled "err_builtin_not_used_as_value",
    205 AddressOfUnaddressable "err_address_of_unaddressable",
    210 DefineNoNewVars "err_define_no_new_vars",
    219 SelectCaseInvalid "err_select_case_invalid",
    300 NoteOwnNewRemoved "note_own_new_removed",
    312 UnusedImport "err_unused_import",
    313 UnusedImportAs "err_unused_import_as",
    377 StructEmbedding "err_struct_embedding",
    454 NoteRefersTo "note_refers_to",
}

impl Code {
    pub fn number(self) -> u16 {
        self as u16
    }

    /// The severity the name's prefix states: `err_`, `warn_` or `note_`.
    pub fn severity(self) -> Severity {
        match self.name().split('_').next() {
            Some("warn") => Severity::Warning,
            Some("note") => Severity::Note,
            _ => Severity::Error,
        }
    }
}

impl fmt::Display for Code {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "E{:04}", self.number())
    }
}

#[cfg(test)]
mod tests {
    use std::env;
    use std::fmt::Write;
    use std::fs;

    use super::*;
    use crate::LockRegistry;

    const LOCK_PATH: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../../docs/errors/codes.lock");

    #[test]
    fn a_code_prints_as_e_and_four_digits() {
        assert_eq!(Code::LexIllegalNul.to_string(), "E0002");
        assert_eq!(Code::NoteGoppModLine.to_string(), "E0073");
    }

    #[test]
    fn severity_follows_the_name() {
        assert_eq!(Code::TooManyErrors.severity(), Severity::Error);
        assert_eq!(Code::NoteGoppModLine.severity(), Severity::Note);
    }

    /// With `GOPP_UPDATE_CODES=1`, codes numbered right after the last row
    /// are appended to the lock file first.
    #[test]
    fn every_code_is_its_row_of_the_lock_file() {
        let mut text = fs::read_to_string(LOCK_PATH).unwrap();
        let mut lock = LockRegistry::parse(&text).unwrap();
        if env::var_os("GOPP_UPDATE_CODES").is_some() {
            let mut new: Vec<Code> = Code::ALL.to_vec();
            new.retain(|code| usize::from(code.number()) > lock.len());
            new.sort();
            for code in new {
                writeln!(text, "{code} {}", code.name()).unwrap();
            }
            lock = LockRegistry::parse(&text).unwrap();
            fs::write(LOCK_PATH, &text).unwrap();
        }
        for &code in Code::ALL {
            assert_eq!(
                lock.name(code.number()),
                Some(code.name()),
                "{code} {} is not a row of docs/errors/codes.lock",
                code.name()
            );
        }
    }

    #[test]
    fn codes_are_listed_in_order_without_repeats() {
        assert!(Code::ALL.windows(2).all(|pair| pair[0] < pair[1]));
    }
}
