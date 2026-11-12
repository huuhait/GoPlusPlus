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
    68 AddrOfCompositeLit "err_addr_of_complit",
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
    115 LabelNotUsed "err_label_not_used",
    116 LabelRedeclared "err_label_redeclared",
    117 LabelUndeclared "err_label_undeclared",
    118 InvalidBreakLabel "err_invalid_break_label",
    119 InvalidContinueLabel "err_invalid_continue_label",
    120 GotoJumpsOver "err_goto_jumps_over",
    121 GotoJumpsInto "err_goto_jumps_into",
    122 NotInLoopBreak "err_not_in_loop_break",
    123 NotInLoopContinue "err_not_in_loop_continue",
    124 FallthroughMisplaced "err_fallthrough_misplaced",
    125 FallthroughFinal "err_fallthrough_final",
    126 FallthroughTypeSwitch "err_fallthrough_type_switch",
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
    145 ArrayLengthNotConstant "err_array_length_not_constant",
    148 InvalidMapKey "err_invalid_map_key",
    156 ReceiverCount "err_receiver_count",
    157 ReceiverMissing "err_receiver_missing",
    159 UseOfUntypedNil "err_use_of_untyped_nil",
    160 CannotConvert "err_cannot_convert",
    163 ValueTruncated "err_value_truncated",
    164 ValueOverflows "err_value_overflows",
    165 GenericWithoutInstantiation "err_generic_without_instantiation",
    166 OperatorNotDefined "err_operator_not_defined",
    167 MismatchedTypes "err_mismatched_types",
    168 CannotCompare "err_cannot_compare",
    169 DivisionByZero "err_division_by_zero",
    170 ShiftCountNegative "err_shift_count_negative",
    171 ShiftOperandInvalid "err_shift_operand_invalid",
    172 ShiftCountInvalid "err_shift_too_large",
    173 ConstantExpressionOverflow "err_constant_expression_overflow",
    174 NotConstant "err_not_constant",
    175 NoValue "err_no_value",
    176 MultiValueInSingle "err_multi_value_in_single",
    177 BuiltinNotCalled "err_builtin_not_used_as_value",
    178 UndefinedFieldOrMethod "err_undefined_field_or_method",
    179 NotEnoughArguments "err_not_enough_arguments",
    180 TooManyArguments "err_too_many_arguments",
    181 NotEnoughReturnValues "err_not_enough_arguments_return",
    182 TooManyReturnValues "err_too_many_arguments_return",
    183 CannotIndex "err_cannot_index",
    184 CannotSlice "err_cannot_slice",
    185 CannotSliceUnaddressable "err_cannot_slice_unaddressable",
    186 SliceOfString "err_slice_3_string",
    187 SliceIndicesInverted "err_slice_indices_inverted",
    188 IndexOutOfBounds "err_index_out_of_bounds",
    189 LiteralTooFew "err_literal_too_few",
    190 LiteralTooMany "err_literal_too_many",
    191 LiteralMixture "err_literal_mixture",
    192 LiteralDuplicateField "err_literal_duplicate_field",
    193 LiteralMissingKey "err_literal_missing_key",
    194 LiteralDuplicateKey "err_literal_duplicate_key",
    195 LiteralDuplicateIndex "err_literal_duplicate_index",
    196 LiteralIndexNotConstant "err_literal_index_not_constant",
    197 LiteralUnexportedField "err_literal_unexported_field",
    198 LiteralInvalidFieldName "err_literal_invalid_field_name",
    199 LiteralArrayTooLong "err_literal_array_too_long",
    200 LiteralMissingType "err_literal_missing_type",
    201 NotAnInterface "err_type_assertion_non_interface",
    204 DerefNonPointer "err_deref_non_pointer",
    205 AddressOfUnaddressable "err_address_of_unaddressable",
    206 CannotAssign "err_cannot_assign",
    207 CannotAssignTo "err_cannot_assign_to",
    208 AssignMismatch "err_assign_mismatch",
    210 DefineNoNewVars "err_define_no_new_vars",
    212 MissingInitExpr "err_missing_init_expr",
    213 ExtraInitExpr "err_extra_init_expr",
    214 MissingReturn "err_missing_return",
    216 DuplicateCase "err_duplicate_case",
    217 DuplicateTypeCase "err_duplicate_type_case",
    218 SwitchTagNotComparable "err_switch_tag_not_comparable",
    219 SelectCaseInvalid "err_select_case_invalid",
    221 GoDeferNeedsCall "err_go_defer_needs_call",
    222 IncDecNonNumeric "err_inc_dec_non_numeric",
    223 BuiltinNotEnoughArguments "err_builtin_not_enough_args",
    224 BuiltinTooManyArguments "err_builtin_too_many_args",
    225 BuiltinInvalidArgument "err_builtin_invalid_arg",
    227 CannotUseEllipsis "err_cannot_use_ellipsis",
    230 InvalidUseOfDots "err_invalid_use_of_dots",
    233 NotAGenericType "err_not_a_generic_type",
    235 IotaOutsideConst "err_iota_outside_const",
    236 InvalidConstantType "err_invalid_constant_type",
    237 MethodExprPointerReceiver "err_method_expr_ptr_recv",
    238 AmbiguousSelector "err_ambiguous_selector",
    266 OwnMoveNotPlace "err_own_move_not_place",
    298 OwnWeakArg "err_own_weak_arg",
    300 NoteOwnNewRemoved "note_own_new_removed",
    312 UnusedImport "err_unused_import",
    313 UnusedImportAs "err_unused_import_as",
    341 MoveNonlocal "err_move_nonlocal",
    347 NilNeverNil "err_nil_never_nil",
    377 StructEmbedding "err_struct_embedding",
    454 NoteRefersTo "note_refers_to",
    455 NoteHave "note_have",
    456 NoteWant "note_want",
    457 UnusedExpression "err_unused_expression",
    458 InvalidIndex "err_invalid_index",
    459 LiteralUnknownField "err_literal_unknown_field",
    460 LiteralInvalidType "err_literal_invalid_type",
    461 ChannelDirection "err_channel_direction",
    463 AssertedTypeParam "err_asserted_type_param",
    464 MalformedConstant "err_malformed_constant",
    465 TildeOutsideConstraint "err_tilde_outside_constraint",
    466 NoteOwnFixHeapPointer "note_own_fix_heap_pointer",
    467 NoteOwnFixTake "note_own_fix_take",
    468 NoteOwnFixOption "note_own_fix_option",
    469 OwnStarOnLeft "err_own_star_on_left",
    470 NoteOwnFixNoStar "note_own_fix_no_star",
    471 OwnCopyToCopy "err_own_copy_to_copy",
    472 NoteOwnFixAddress "note_own_fix_address",
    473 TypeTooLarge "err_type_too_large",
    474 NonBooleanCondition "err_non_boolean_condition",
    475 MultipleDefaults "err_multiple_defaults",
    476 NotePreviousCase "note_previous_case",
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
