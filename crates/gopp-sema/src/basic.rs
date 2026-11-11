//! The basic types, the kinds of untyped constants, and which constants a
//! basic type can hold.

use crate::constant::{BinaryOp, Kind, Value};

/// A basic type, or the kind of an untyped constant.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Basic {
    Bool,
    Int,
    Int8,
    Int16,
    Int32,
    Int64,
    Uint,
    Uint8,
    Uint16,
    Uint32,
    Uint64,
    Uintptr,
    Float32,
    Float64,
    Complex64,
    Complex128,
    String,
    UntypedBool,
    UntypedInt,
    UntypedRune,
    UntypedFloat,
    UntypedComplex,
    UntypedString,
    UntypedNil,
}

impl Basic {
    pub fn is_untyped(self) -> bool {
        use Basic::*;
        matches!(
            self,
            UntypedBool
                | UntypedInt
                | UntypedRune
                | UntypedFloat
                | UntypedComplex
                | UntypedString
                | UntypedNil
        )
    }

    /// The type a constant of this untyped kind has where no type is asked
    /// for. A type is its own default.
    pub fn default_type(self) -> Self {
        match self {
            Self::UntypedBool => Self::Bool,
            Self::UntypedInt => Self::Int,
            Self::UntypedRune => Self::Int32,
            Self::UntypedFloat => Self::Float64,
            Self::UntypedComplex => Self::Complex128,
            Self::UntypedString => Self::String,
            other => other,
        }
    }

    /// For an integer type its bits and whether it has a sign. `int`,
    /// `uint` and `uintptr` have 64 bits on every target.
    fn integer(self) -> Option<(u32, bool)> {
        Some(match self {
            Self::Int8 => (8, true),
            Self::Int16 => (16, true),
            Self::Int32 => (32, true),
            Self::Int | Self::Int64 => (64, true),
            Self::Uint8 => (8, false),
            Self::Uint16 => (16, false),
            Self::Uint32 => (32, false),
            Self::Uint | Self::Uint64 | Self::Uintptr => (64, false),
            _ => return None,
        })
    }
}

/// A float as the type of `single` or double precision holds it: rounded
/// in one step. `None` past the largest number of the type.
fn rounded_float(x: &Value, single: bool) -> Option<Value> {
    let nearest = if single { x.to_f32() } else { x.to_f64() };
    nearest.is_finite().then(|| Value::from_f64(nearest))
}

/// `x` as a constant of the type `ty` holds it: an integer for an integer
/// type, a float rounded to the precision of a float type. `None` if the
/// type cannot hold it. An unknown value fits every type, so that one
/// error is not reported twice.
pub fn representable(x: &Value, ty: Basic) -> Option<Value> {
    use Basic::*;
    if x.kind() == Kind::Unknown {
        return Some(Value::Unknown);
    }
    match ty {
        Bool | UntypedBool => (x.kind() == Kind::Bool).then(|| x.clone()),
        String | UntypedString => (x.kind() == Kind::String).then(|| x.clone()),
        UntypedInt | UntypedRune => Some(x.to_int()).filter(|x| x.kind() == Kind::Int),
        UntypedFloat => Some(x.to_float()).filter(|x| x.kind() == Kind::Float),
        UntypedComplex => Some(x.to_complex()).filter(|x| x.kind() == Kind::Complex),
        Float32 | Float64 => {
            let x = Some(x.to_float()).filter(|x| x.kind() == Kind::Float)?;
            rounded_float(&x, ty == Float32)
        }
        Complex64 | Complex128 => {
            let x = Some(x.to_complex()).filter(|x| x.kind() == Kind::Complex)?;
            let re = rounded_float(&x.real(), ty == Complex64)?;
            let im = rounded_float(&x.imag(), ty == Complex64)?;
            Some(re.binary(BinaryOp::Add, &im.make_imag()))
        }
        UntypedNil => None,
        _ => {
            let (bits, signed) = ty.integer()?;
            let x = Some(x.to_int()).filter(|x| x.kind() == Kind::Int)?;
            // A signed type holds one more negative number than positive:
            // -2^(bits-1), whose magnitude has `bits` bits.
            let fits = match (signed, x.sign() < 0) {
                (true, false) => x.bit_len() < u64::from(bits),
                (true, true) => {
                    let above = x.binary(BinaryOp::Add, &Value::Int64(1));
                    above.bit_len() < u64::from(bits)
                }
                (false, negative) => !negative && x.bit_len() <= u64::from(bits),
            };
            fits.then_some(x)
        }
    }
}

#[cfg(test)]
mod tests {
    use gopp_syntax::ast::LitKind;

    use super::*;

    fn value(text: &str) -> Value {
        let (kind, text) = match text.as_bytes()[0] {
            b'"' => (LitKind::String, text),
            _ if text.ends_with('i') => (LitKind::Imag, text),
            _ if text.contains(['.', 'e', 'p']) => (LitKind::Float, text),
            _ => (LitKind::Int, text.trim_start_matches('-')),
        };
        Value::from_literal(text, kind)
    }

    fn fits(text: &str, ty: Basic) -> bool {
        let negative = text.starts_with('-') && !text.contains(['.', 'e']);
        let mut x = value(text);
        if negative {
            x = x.unary(crate::constant::UnaryOp::Neg, 0);
        }
        representable(&x, ty).is_some()
    }

    #[test]
    fn an_integer_type_holds_the_integers_of_its_range() {
        use Basic::*;
        let cases = [
            (Int8, "127", true),
            (Int8, "128", false),
            (Int8, "-128", true),
            (Int8, "-129", false),
            (Uint8, "255", true),
            (Uint8, "256", false),
            (Uint8, "-1", false),
            (Int16, "-32768", true),
            (Int32, "2147483648", false),
            (Int32, "-2147483648", true),
            (Int64, "9223372036854775807", true),
            (Int64, "9223372036854775808", false),
            (Int64, "-9223372036854775808", true),
            (Int64, "-9223372036854775809", false),
            (Int, "-9223372036854775808", true),
            (Uint64, "18446744073709551615", true),
            (Uint64, "18446744073709551616", false),
            (Uintptr, "18446744073709551615", true),
            (Uint, "-1", false),
            (UntypedInt, "123456789012345678901234567890", true),
            // A float that is an integer is one.
            (Int8, "1e2", true),
            (Int8, "1.5", false),
            (Uint8, "0i", true),
            (Int, "1i", false),
            (Int, "\"1\"", false),
        ];
        for (ty, text, expected) in cases {
            assert_eq!(fits(text, ty), expected, "{text} as {ty:?}");
        }
    }

    #[test]
    fn a_float_type_holds_a_number_rounded_to_it() {
        use Basic::*;
        let rounded = |text: &str, ty| representable(&value(text), ty).map(|x| x.exact_string());
        assert_eq!(rounded("0.5", Float32).as_deref(), Some("1/2"));
        assert_eq!(
            rounded("0.1", Float32).as_deref(),
            Some("13421773/134217728")
        );
        assert_eq!(
            rounded("0.1", Float64).as_deref(),
            Some("3602879701896397/36028797018963968")
        );
        assert_eq!(rounded("0.1", UntypedFloat).as_deref(), Some("1/10"));
        assert_eq!(rounded("1", Float64).as_deref(), Some("1"));
        assert_eq!(rounded("1e39", Float32), None);
        assert_eq!(
            rounded("1e39", Float64).as_deref(),
            Some("999999999999999939709166371603178586112")
        );
        assert_eq!(rounded("1e309", Float64), None);
        assert_eq!(rounded("1e-400", Float64).as_deref(), Some("0"));
        assert_eq!(rounded("1i", Float64), None);
        assert_eq!(
            rounded("0.1i", Complex64).as_deref(),
            Some("(0 + 13421773/134217728i)")
        );
        assert_eq!(rounded("1e39i", Complex64), None);
        assert_eq!(
            rounded("1e39i", Complex128).map(|x| x.len() > 10),
            Some(true)
        );
        assert_eq!(rounded("\"a\"", Float64), None);
    }

    #[test]
    fn other_kinds_fit_their_own_types_only() {
        use Basic::*;
        assert!(representable(&Value::Bool(true), Bool).is_some());
        assert!(representable(&Value::Bool(true), Int).is_none());
        assert!(representable(&Value::string(b"a"), String).is_some());
        assert!(representable(&Value::string(b"a"), Bool).is_none());
        assert!(representable(&Value::Int64(0), UntypedNil).is_none());
        // What is unknown was reported where it arose.
        assert_eq!(representable(&Value::Unknown, Int8), Some(Value::Unknown));
        assert_eq!(UntypedRune.default_type(), Int32);
        assert_eq!(UntypedFloat.default_type(), Float64);
        assert_eq!(Uint8.default_type(), Uint8);
        assert!(UntypedNil.is_untyped() && !Int.is_untyped());
    }
}
