//! Numbers as C writes them, and the operations whose results Go
//! defines where C does not (EXP-8).

use std::fmt::Write;

/// The integer types: their bits, and whether they have a sign.
const INTS: [(u8, bool); 8] = [
    (8, true),
    (16, true),
    (32, true),
    (64, true),
    (8, false),
    (16, false),
    (32, false),
    (64, false),
];

/// `int8_t` to `uint64_t`.
pub(crate) fn int_type(bits: u8, signed: bool) -> String {
    format!("{}int{bits}_t", if signed { "" } else { "u" })
}

/// What a helper of the type has in its name: `i8` to `u64`.
pub(crate) fn int_tag(bits: u8, signed: bool) -> String {
    format!("{}{bits}", if signed { "i" } else { "u" })
}

/// The integer of `bits` bits whose bits are the low bits of `value`.
pub(crate) fn int_literal(value: u64, bits: u8, signed: bool) -> String {
    let mut digits = itoa::Buffer::new();
    if !signed {
        let masked = match bits {
            64 => value,
            bits => value & ((1 << bits) - 1),
        };
        return match bits {
            64 => format!("{}ull", digits.format(masked)),
            bits => format!("(({}){})", int_type(bits, false), digits.format(masked)),
        };
    }
    // The value with its sign, from the bits that the type has.
    let unused = 64 - u32::from(bits.min(64));
    let whole = ((value << unused) as i64) >> unused;
    match (bits, whole) {
        // C has no literal of the least integer.
        (64, i64::MIN) => "(-9223372036854775807ll - 1)".to_owned(),
        (64, whole) if whole < 0 => format!("({}ll)", digits.format(whole)),
        (64, whole) => format!("{}ll", digits.format(whole)),
        (bits, whole) => format!("(({}){})", int_type(bits, true), digits.format(whole)),
    }
}

/// The float with the bits, as a hexadecimal literal, which says it
/// exactly; an infinity or a NaN by its bits.
pub(crate) fn float_literal(bits: u64, single: bool) -> String {
    let (sign, exponent, fraction, digits, bias, suffix) = match single {
        true => (
            bits >> 31 & 1,
            (bits >> 23 & 0xff) as i64,
            (bits & 0x7f_ffff) << 1,
            6,
            127,
            "f",
        ),
        false => (
            bits >> 63,
            (bits >> 52 & 0x7ff) as i64,
            bits & 0xf_ffff_ffff_ffff,
            13,
            1023,
            "",
        ),
    };
    let largest = 2 * bias + 1;
    let magnitude = match exponent {
        _ if exponent == largest => {
            return match single {
                true => format!("gopp_f32({:#x}u)", bits & 0xffff_ffff),
                false => format!("gopp_f64({bits:#x}ull)"),
            };
        }
        0 if fraction == 0 => format!("0x0p+0{suffix}"),
        0 => format!("0x0.{fraction:0digits$x}p{}{suffix}", 1 - bias),
        _ => format!("0x1.{fraction:0digits$x}p{:+}{suffix}", exponent - bias),
    };
    match sign {
        0 => magnitude,
        _ => format!("(-{magnitude})"),
    }
}

/// The helpers that every unit has: a shift by any count, a division
/// that does not trap on the least integer, and a conversion from a
/// float that stays in the range of the integer.
pub(crate) fn helpers(out: &mut String) {
    for (bits, signed) in INTS {
        let (ty, tag) = (int_type(bits, signed), int_tag(bits, signed));
        let unsigned = int_type(bits, false);
        let start = format!("[[maybe_unused]] static inline {ty} gopp_");
        let _ = writeln!(
            out,
            "{start}shl_{tag}({ty} x, uint64_t count) {{\n  \
             return count >= {bits} ? ({ty})0 : ({ty})(({unsigned})x << count);\n}}"
        );
        let shifted = match signed {
            true => format!("({ty})(x >> (count >= {bits} ? {} : count))", bits - 1),
            false => format!("count >= {bits} ? ({ty})0 : ({ty})(x >> count)"),
        };
        let _ = writeln!(
            out,
            "{start}shr_{tag}({ty} x, uint64_t count) {{\n  return {shifted};\n}}"
        );
        if signed {
            let _ = writeln!(
                out,
                "{start}div_{tag}({ty} x, {ty} y) {{\n  \
                 return y == -1 ? ({ty})(0 - ({unsigned})x) : ({ty})(x / y);\n}}\n\
                 {start}rem_{tag}({ty} x, {ty} y) {{\n  \
                 return y == -1 ? ({ty})0 : ({ty})(x % y);\n}}"
            );
        }
    }
    for (float, from, suffix) in [("float", "f32", "f"), ("double", "f64", "")] {
        for (bits, signed) in INTS {
            let (ty, tag) = (int_type(bits, signed), int_tag(bits, signed));
            // The first float past the largest integer, and the least
            // integer, which a float holds exactly.
            let (past, least, most) = match signed {
                true => (
                    format!("0x1p+{}{suffix}", bits - 1),
                    format!(
                        "x < -0x1p+{}{suffix} ? {}",
                        bits - 1,
                        int_literal(1 << (bits - 1), bits, true)
                    ),
                    int_literal((1 << (bits - 1)) - 1, bits, true),
                ),
                false => (
                    format!("0x1p+{bits}{suffix}"),
                    format!("x < 0 ? ({ty})0"),
                    int_literal(u64::MAX, bits, false),
                ),
            };
            let _ = writeln!(
                out,
                "[[maybe_unused]] static inline {ty} gopp_{from}_to_{tag}({float} x) {{\n  \
                 return x != x ? ({ty})0 : x >= {past} ? {most} : {least} : ({ty})x;\n}}"
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_integer_is_written_with_its_sign_and_its_width() {
        assert_eq!(int_literal(5, 64, true), "5ll");
        assert_eq!(int_literal(u64::MAX, 64, true), "(-1ll)");
        assert_eq!(
            int_literal(1 << 63, 64, true),
            "(-9223372036854775807ll - 1)"
        );
        assert_eq!(int_literal(u64::MAX, 64, false), "18446744073709551615ull");
        assert_eq!(int_literal(0x80, 8, true), "((int8_t)-128)");
        assert_eq!(int_literal(0x1ff, 8, false), "((uint8_t)255)");
        assert_eq!(int_literal(0xffff, 16, true), "((int16_t)-1)");
        assert_eq!(int_literal(7, 32, false), "((uint32_t)7)");
    }

    #[test]
    fn a_float_is_written_exactly() {
        let double = |value: f64| float_literal(value.to_bits(), false);
        let single = |value: f32| float_literal(u64::from(value.to_bits()), true);
        assert_eq!(double(1.5), "0x1.8000000000000p+0");
        assert_eq!(double(-0.0), "(-0x0p+0)");
        assert_eq!(double(0.1), "0x1.999999999999ap-4");
        assert_eq!(double(f64::MIN_POSITIVE / 2.0), "0x0.8000000000000p-1022");
        assert_eq!(double(f64::MAX), "0x1.fffffffffffffp+1023");
        assert_eq!(double(f64::INFINITY), "gopp_f64(0x7ff0000000000000ull)");
        assert_eq!(double(f64::NAN), "gopp_f64(0x7ff8000000000000ull)");
        assert_eq!(single(1.5), "0x1.800000p+0f");
        assert_eq!(single(-2.0), "(-0x1.000000p+1f)");
        assert_eq!(single(f32::MIN_POSITIVE / 2.0), "0x0.800000p-126f");
        assert_eq!(single(f32::NEG_INFINITY), "gopp_f32(0xff800000u)");
    }
}
