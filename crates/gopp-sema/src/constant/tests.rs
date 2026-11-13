use gopp_syntax::ast::LitKind;
use gopp_testutil::linear;

use super::*;

fn int(text: &str) -> Value {
    Value::from_literal(text, LitKind::Int)
}

fn float(text: &str) -> Value {
    Value::from_literal(text, LitKind::Float)
}

fn equal(x: &Value, y: &Value) -> bool {
    x.compare(CompareOp::Eq, y)
}

/// Evaluates an expression in postfix notation.
/// `None` where an operand that the notation needs is missing.
fn eval(line: &str) -> Option<Value> {
    let binary = |part: &str| {
        Some(match part {
            "+" => BinaryOp::Add,
            "-" => BinaryOp::Sub,
            "*" => BinaryOp::Mul,
            "/" => BinaryOp::Quo,
            "//" => BinaryOp::IntQuo,
            "%" => BinaryOp::Rem,
            "&" => BinaryOp::And,
            "|" => BinaryOp::Or,
            "^" => BinaryOp::Xor,
            "&^" => BinaryOp::AndNot,
            "&&" => BinaryOp::LogicalAnd,
            "||" => BinaryOp::LogicalOr,
            _ => return None,
        })
    };
    let comparison = |part: &str| {
        Some(match part {
            "==" => CompareOp::Eq,
            "!=" => CompareOp::Ne,
            "<" => CompareOp::Lt,
            "<=" => CompareOp::Le,
            ">" => CompareOp::Gt,
            ">=" => CompareOp::Ge,
            _ => return None,
        })
    };
    let mut stack: Vec<Value> = Vec::new();
    for part in line.split_whitespace() {
        let literal = part.split_once(':').filter(|(tag, _)| tag.len() == 1);
        let value = if let Some((tag, text)) = literal {
            match tag {
                "I" => Value::from_literal(text, LitKind::Int),
                "F" => Value::from_literal(text, LitKind::Float),
                "M" => Value::from_literal(text, LitKind::Imag),
                "R" => Value::from_literal(text, LitKind::Rune),
                "B" => Value::Bool(text == "true"),
                "S" => {
                    let byte = |i| u8::from_str_radix(&text[i..i + 2], 16).unwrap();
                    let bytes: Vec<u8> = (0..text.len()).step_by(2).map(byte).collect();
                    Value::string(&bytes)
                }
                _ => panic!("unknown literal {part}"),
            }
        } else if let Some(op) = binary(part) {
            let (y, x) = (stack.pop()?, stack.pop()?);
            x.binary(op, &y)
        } else if let Some(op) = comparison(part) {
            let (y, x) = (stack.pop()?, stack.pop()?);
            Value::Bool(x.compare(op, &y))
        } else if let Some(count) = part.strip_prefix("<<") {
            stack.pop()?.shift(true, count.parse().unwrap())
        } else if let Some(count) = part.strip_prefix(">>") {
            stack.pop()?.shift(false, count.parse().unwrap())
        } else if let Some(bits) = part.strip_prefix("comp:") {
            stack
                .pop()?
                .unary(UnaryOp::Complement, bits.parse().unwrap())
        } else {
            let x = stack.pop()?;
            match part {
                "neg" => x.unary(UnaryOp::Neg, 0),
                "pos" => x.unary(UnaryOp::Plus, 0),
                "not" => x.unary(UnaryOp::Not, 0),
                "comp" => x.unary(UnaryOp::Complement, 0),
                "int" => x.to_int(),
                "float" => x.to_float(),
                "complex" => x.to_complex(),
                "real" => x.real(),
                "imag" => x.imag(),
                "makeimag" => x.make_imag(),
                _ => panic!("unknown part {part}"),
            }
        };
        stack.push(value);
    }
    stack.pop()
}

/// A value on one line: its kind, its exact value and its short text.
fn shown(value: &Value) -> String {
    let kind = match value.kind() {
        Kind::Unknown => "Unknown",
        Kind::Bool => "Bool",
        Kind::String => "String",
        Kind::Int => "Int",
        Kind::Float => "Float",
        Kind::Complex => "Complex",
        Kind::Composite => "Composite",
    };
    let mut out = format!("{kind} {} | {value}", value.exact_string());
    if matches!(value.kind(), Kind::Int | Kind::Float) {
        let double = value.to_f64().to_bits();
        let single = (value.to_f32() as f32).to_bits();
        out += &format!(" | {double:016x} | {single:08x}");
    }
    out
}

fn show(line: &str) -> String {
    shown(&eval(line).unwrap())
}

#[test]
fn decimal_fractions_are_exact() {
    assert_eq!(
        show("F:0.3 F:0.1 - F:0.2 -"),
        "Float 0 | 0 | 0000000000000000 | 00000000"
    );
    assert_eq!(show("F:0.1 F:0.2 + F:0.3 =="), "Bool true | true");
    assert_eq!(
        show("F:1e-400 F:1e400 *"),
        "Float 1 | 1 | 3ff0000000000000 | 3f800000"
    );
    assert_eq!(
        show("I:1 I:3 / I:3 *"),
        "Float 1 | 1 | 3ff0000000000000 | 3f800000"
    );
    assert_eq!(
        show("I:1 I:3 /"),
        "Float 1/3 | 0.333333 | 3fd5555555555555 | 3eaaaaab"
    );
}

#[test]
fn integers_have_any_size() {
    assert_eq!(
        show("I:1 <<100 neg comp"),
        "Int 1267650600228229401496703205375 | 1267650600228229401496703205375 | \
         4630000000000000 | 71800000"
    );
    assert_eq!(
        eval("I:9223372036854775807 I:1 +"),
        Some(int("9223372036854775808"))
    );
    assert_eq!(
        eval("I:9223372036854775808 neg"),
        Some(Value::Int64(i64::MIN))
    );
    assert_eq!(
        eval("I:9223372036854775808 neg I:1 neg //"),
        Some(int("9223372036854775808"))
    );
    assert_eq!(eval("I:7 neg I:2 //"), Some(Value::Int64(-3)));
    assert_eq!(eval("I:7 neg I:2 %"), Some(Value::Int64(-1)));
    assert_eq!(eval("I:7 neg >>1"), Some(Value::Int64(-4)));
    assert_eq!(eval("I:0 comp:8"), Some(Value::Int64(255)));
    assert_eq!(
        eval("I:1 <<64 I:1 - I:0xff00 &^ >>60"),
        Some(Value::Int64(15))
    );
}

#[test]
fn what_cannot_be_computed_is_unknown() {
    for line in [
        "I:1 I:0 /",
        "I:1 I:0 //",
        "I:1 I:0 %",
        "F:1.5 F:0 /",
        "F:1e+1000000000",
        "F:6e5518446744",
        "I:0 F:1e+1000000000 *",
        "F:1.5 int",
        "M:1i float",
        "B:true I:1 +",
        "S:61 I:1 +",
        "F:1.5 F:2 %",
        "B:true neg",
        "I:1 not",
        "F:1.5 comp",
    ] {
        assert_eq!(eval(line), Some(Value::Unknown), "{line}");
    }
    // An unknown operand is handed on, and compares as nothing.
    assert_eq!(eval("I:1 I:0 / I:5 +"), Some(Value::Unknown));
    assert_eq!(eval("I:1 I:0 / I:5 =="), Some(Value::Bool(false)));
    assert_eq!(eval("I:1 I:0 / I:5 !="), Some(Value::Bool(false)));
    assert_eq!(Value::Unknown.sign(), 1);
}

/// `literal = same value`, from the tests of Go's `go/constant`.
#[test]
fn number_literals_are_read_as_go_reads_them() {
    let ints = [
        "0_123 = 0123",
        "0123_456 = 0123456",
        "1_234_567 = 1234567",
        "0X_CAFE_f00d = 0xcafef00d",
        "0o1234 = 01234",
        "0O0123_4567 = 01234567",
        "0b1011 = 0xb",
        "0B_0010_1101 = 0x2d",
        "017 = 15",
        "0 = 0x0",
    ];
    for case in ints {
        let (literal, same) = case.split_once(" = ").unwrap();
        assert!(equal(&int(literal), &int(same)), "{case}");
    }
    let floats = [
        "1_2_3. = 123.",
        "0_123. = 123.",
        "0_0e0 = 0.",
        "0123E1_2_3 = 123e123",
        "123.E-1_0 = 123e-10",
        ".0123E123 = .0123e123",
        "0123.01_23 = 123.0123",
        "1e-1000000000 = 0",
        "0x0.p+0 = 0.",
        "0x.1p-0 = 0.0625",
        "0x1234.P84 = 0x1234000000000000000000000.p0",
        "0Xdead.cafep+1 = 0x1bd5b95fc.p-16",
        "0x12_34.P1_2_3 = 0x1234.p123",
    ];
    for case in floats {
        let (literal, same) = case.split_once(" = ").unwrap();
        let (x, y) = (float(literal), float(same));
        assert_eq!(x.kind(), Kind::Float, "{case}");
        assert!(equal(&x, &y), "{case}: {x} and {y}");
    }
    // An imaginary literal with a leading zero is decimal.
    let imaginary = |text| Value::from_literal(text, LitKind::Imag);
    assert!(equal(&imaginary("0123.i"), &imaginary("123i")));
    assert!(equal(&imaginary("0123i"), &imaginary("123i")));
    assert_eq!(imaginary("1e+1000000000i"), Value::Unknown);
    assert_eq!(imaginary("12"), Value::Unknown);
}

#[test]
fn a_fraction_too_large_to_stay_exact_becomes_a_float() {
    // 10^1233 has more than 4096 bits.
    assert!(matches!(float("1e1232"), Value::Rat(_)));
    assert!(matches!(float("1e1234"), Value::Float(_)));
    assert!(matches!(eval("F:1e700 F:1e700 *"), Some(Value::Float(_))));
    assert!(matches!(eval("I:1 <<5000 float"), Some(Value::Float(_))));
    // The float is close, not exact: this is 10 only after rounding.
    assert_eq!(
        show("F:1e2000 F:1e1999 /"),
        "Float 0x.ap+4 | 10 | 4024000000000000 | 41200000"
    );
    assert_eq!(show("F:1e5000").split(" | ").nth(1), Some("1e+5000"));
    assert_eq!(show("F:1e-5000 neg").split(" | ").nth(1), Some("-1e-5000"));
    // Go's own way of writing a small number, kept for equal messages.
    assert_eq!(
        show("F:894562232.7516972e-4245").split(" | ").nth(1),
        Some("0.894562e-4236")
    );
    // A float that is an integer but for its last bits converts to it.
    assert_eq!(eval("F:1e2000 F:1e1999 / int"), Some(Value::Int64(10)));
}

#[test]
fn rounding_to_a_float_type_takes_one_step() {
    let double = |line: &str| eval(line).unwrap().to_f64().to_bits();
    let single = |line: &str| (eval(line).unwrap().to_f32() as f32).to_bits();
    assert_eq!(double("F:0.1"), 0.1f64.to_bits());
    assert_eq!(double("F:0.5"), 0.5f64.to_bits());
    assert_eq!(single("F:0.1"), 0.1f32.to_bits());
    // A tie goes to the even mantissa: 2^53 + 1 and 2^53 + 3.
    assert_eq!(double("I:9007199254740993"), 9007199254740992f64.to_bits());
    assert_eq!(double("I:9007199254740995"), 9007199254740996f64.to_bits());
    // Just above a tie of `f32`. Rounding to `f64` first would give the
    // tie itself, and the wrong `f32`.
    let above_tie = "I:1 I:1 <<24 / I:1 + I:1 I:1 <<60 / +";
    assert_eq!(single("I:1 I:1 <<24 / I:1 +"), 1f32.to_bits());
    assert_eq!(single(above_tie), 1.0000001f32.to_bits());
    // The smallest numbers: subnormal, half of the smallest, and less.
    assert_eq!(double("F:5e-324"), 1);
    assert_eq!(double("F:0x1p-1074"), 1);
    assert_eq!(double("F:0x1p-1075"), 0);
    assert_eq!(double("F:0x1.8p-1075"), 1);
    assert_eq!(double("F:0x1.8p-1074"), 2);
    assert_eq!(double("F:0x2.8p-1074"), 2);
    assert_eq!(double("F:0x1.0000000000001p-1075"), 1);
    assert_eq!(double("F:0x0.fffffffffffff8p-1022"), 1 << 52);
    assert_eq!(single("F:0x1p-149"), 1);
    assert_eq!(single("F:0x1p-150"), 0);
    assert_eq!(single("F:1e-46"), 0);
    // The largest numbers, and the first that round to an infinity.
    assert_eq!(double("F:0x1.fffffffffffffp1023"), f64::MAX.to_bits());
    assert_eq!(double("F:0x1.fffffffffffff7p1023"), f64::MAX.to_bits());
    assert_eq!(double("F:0x1.fffffffffffff8p1023"), f64::INFINITY.to_bits());
    assert_eq!(single("F:0x1.fffffep127"), f32::MAX.to_bits());
    assert_eq!(single("F:0x1.ffffffp127"), f32::INFINITY.to_bits());
    assert_eq!(double("F:1e5000 neg"), f64::NEG_INFINITY.to_bits());
    assert_eq!(double("F:1e-5000 neg"), (-0.0f64).to_bits());
}

#[test]
fn a_value_is_shown_as_go_shows_it() {
    let cases = [
        ("F:1e100", "1e+100"),
        ("F:123456.789", "123456.789"),
        ("F:1234567.5", "1.23457e+06"),
        ("F:0.000012345", "1.2345e-05"),
        ("F:0.00012345", "0.00012345"),
        ("F:100000", "100000"),
        ("F:1000000", "1e+06"),
        ("I:1 I:3 / neg", "-0.333333"),
        ("M:2i F:1.5 +", "(1.5 + 2i)"),
        ("S:61220a S:5c +", "\"a\\\"\\n\\\\\""),
        ("B:true not", "false"),
    ];
    for (line, text) in cases {
        assert_eq!(eval(line).unwrap().to_string(), text, "{line}");
    }
    let long = Value::string("x".repeat(100).as_bytes());
    assert_eq!(long.to_string(), format!("\"{}...", "x".repeat(68)));
    assert_eq!(long.exact_string().len(), 102);
}

#[test]
fn strings_join_and_compare_by_their_bytes() {
    assert_eq!(eval("S:666f6f S:626172 +"), Some(Value::string(b"foobar")));
    assert_eq!(eval("S:666f6f S:626172 <"), Some(Value::Bool(false)));
    assert_eq!(eval("S:ff S:c3a9 >"), Some(Value::Bool(true)));
    assert_eq!(eval("S: S: =="), Some(Value::Bool(true)));
}

#[test]
fn a_long_chain_of_joins_is_joined_and_dropped_without_recursion() {
    let part = Value::string(b"ab");
    let mut chain = part.clone();
    for _ in 0..200_000 {
        chain = chain.binary(BinaryOp::Add, &part);
    }
    let Value::Str(text) = &chain else {
        panic!("not a string");
    };
    assert_eq!(text.bytes().len(), 400_002);
}

#[test]
fn the_work_of_a_chain_grows_with_its_length_not_faster() {
    // Each product is a fraction of bounded size, then a float.
    let products = |count: usize| {
        let before = work();
        let ten = float("1e10");
        let mut x = float("1");
        for _ in 0..count {
            x = x.binary(BinaryOp::Mul, &ten);
        }
        work() - before
    };
    let sums = |count: usize| {
        let before = work();
        let third = eval("I:1 I:3 /").unwrap();
        let mut x = float("1e-300");
        for _ in 0..count {
            x = x.binary(BinaryOp::Add, &third);
        }
        work() - before
    };
    let joins = |count: usize| {
        let before = work();
        let part = Value::string(b"abc");
        let mut x = part.clone();
        for _ in 0..count {
            x = x.binary(BinaryOp::Add, &part);
        }
        let Value::Str(text) = &x else {
            panic!("not a string");
        };
        assert_eq!(text.bytes().len(), 3 * count + 3);
        work() - before
    };
    let sizes = [500, 1_000, 2_000, 4_000];
    linear::assert_near_linear(&sizes, products);
    linear::assert_near_linear(&sizes, sums);
    linear::assert_near_linear(&sizes, joins);
}
