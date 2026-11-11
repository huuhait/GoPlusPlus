//! Constant values and their exact arithmetic, as Go's `go/constant`
//! computes them.
//!
//! An integer has any size. A number that is no integer is an exact
//! fraction while its numerator and denominator stay under 4096 bits, and a
//! binary float with 512 bits of mantissa past that. A value that cannot
//! be computed (a number too large, an operand that was in error) is
//! [`Value::Unknown`], which every operation hands on.

mod float;
mod rat;
#[cfg(test)]
mod tests;

use std::cell::Cell;
use std::cmp::Ordering;
use std::fmt;
use std::sync::{Arc, OnceLock};

use gopp_syntax::ast::LitKind;
use num_bigint::{BigInt, BigUint, Sign};

pub use float::Float;
use float::{BINARY32, BINARY64, Ieee, Rounding};
pub use rat::Rat;

use crate::literal::{unquote, unquote_rune};

/// The bits of mantissa of a float constant.
const PREC: u32 = 512;
/// A fraction is exact while its parts have fewer bits than this, and a
/// float is written as a fraction while its exponent is under it.
const MAX_EXP: i64 = 4 << 10;

thread_local! {
    static WORK: Cell<u64> = const { Cell::new(0) };
}

/// Counts the bits an operation handles: the measure of work in the
/// scaling tests.
fn charge(bits: u64) {
    WORK.with(|work| work.set(work.get() + bits));
}

#[cfg(test)]
fn work() -> u64 {
    WORK.with(Cell::get)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Unknown,
    Bool,
    String,
    Int,
    Float,
    Complex,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Unknown,
    Bool(bool),
    Str(Str),
    /// An integer that fits in 64 bits.
    Int64(i64),
    /// An integer that does not.
    Int(BigInt),
    Rat(Rat),
    Float(Float),
    /// The real and the imaginary part: each an integer or a float.
    Complex(Box<Value>, Box<Value>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnaryOp {
    Plus,
    Neg,
    /// `^x`.
    Complement,
    /// `!x`.
    Not,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BinaryOp {
    Add,
    Sub,
    Mul,
    /// The exact quotient, also of two integers.
    Quo,
    /// The quotient of two integers, cut off toward zero.
    IntQuo,
    Rem,
    And,
    Or,
    Xor,
    AndNot,
    LogicalAnd,
    LogicalOr,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompareOp {
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
}

impl CompareOp {
    fn holds(self, order: Ordering) -> bool {
        match self {
            Self::Eq => order == Ordering::Equal,
            Self::Ne => order != Ordering::Equal,
            Self::Lt => order == Ordering::Less,
            Self::Le => order != Ordering::Greater,
            Self::Gt => order == Ordering::Greater,
            Self::Ge => order != Ordering::Less,
        }
    }
}

// ---- strings

/// A string constant: any bytes. Joining two takes one step however long
/// they are; the bytes are put together when they are asked for.
#[derive(Clone)]
pub struct Str(Arc<StrNode>);

enum StrNode {
    Leaf(Box<[u8]>),
    Join {
        // `None` only while the node is being dropped.
        left: Option<Str>,
        right: Option<Str>,
        joined: OnceLock<Box<[u8]>>,
    },
}

impl Str {
    pub fn new(bytes: &[u8]) -> Self {
        Self(Arc::new(StrNode::Leaf(bytes.into())))
    }

    fn join(&self, other: &Self) -> Self {
        charge(1);
        Self(Arc::new(StrNode::Join {
            left: Some(self.clone()),
            right: Some(other.clone()),
            joined: OnceLock::new(),
        }))
    }

    pub fn bytes(&self) -> &[u8] {
        match &*self.0 {
            StrNode::Leaf(bytes) => bytes,
            StrNode::Join { joined, .. } => joined.get_or_init(|| {
                // A chain of joins is as deep as it is long: no recursion.
                let mut out = Vec::new();
                let mut pending = vec![self];
                while let Some(part) = pending.pop() {
                    match &*part.0 {
                        StrNode::Leaf(bytes) => {
                            charge(bytes.len() as u64);
                            out.extend_from_slice(bytes);
                        }
                        StrNode::Join {
                            left,
                            right,
                            joined,
                        } => match joined.get() {
                            Some(bytes) => out.extend_from_slice(bytes),
                            None => pending.extend(right.iter().chain(left)),
                        },
                    }
                }
                out.into()
            }),
        }
    }
}

/// Moves the parts of `node` to `pending`, if nothing else holds the node.
fn take_parts(node: &mut Arc<StrNode>, pending: &mut Vec<Str>) {
    if let Some(StrNode::Join { left, right, .. }) = Arc::get_mut(node) {
        pending.extend(left.take());
        pending.extend(right.take());
    }
}

impl Drop for Str {
    fn drop(&mut self) {
        // A chain of joins is as deep as it is long: no recursion.
        let mut pending = Vec::new();
        take_parts(&mut self.0, &mut pending);
        while let Some(mut part) = pending.pop() {
            take_parts(&mut part.0, &mut pending);
        }
    }
}

impl PartialEq for Str {
    fn eq(&self, other: &Self) -> bool {
        self.bytes() == other.bytes()
    }
}

impl fmt::Debug for Str {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}", String::from_utf8_lossy(self.bytes()))
    }
}

/// Whether `c` is one of the common characters that format text and show
/// as nothing: zero-width marks, direction marks, the byte order mark.
fn is_format(c: char) -> bool {
    matches!(c, '\u{ad}' | '\u{200b}'..='\u{200f}' | '\u{202a}'..='\u{202e}' | '\u{2060}'..='\u{2064}' | '\u{feff}')
}

/// The text between double quotes, in Go's spelling.
fn quoted(bytes: &[u8]) -> String {
    let mut out = String::from("\"");
    for chunk in bytes.utf8_chunks() {
        for c in chunk.valid().chars() {
            match c {
                '"' => out.push_str("\\\""),
                '\\' => out.push_str("\\\\"),
                '\x07' => out.push_str("\\a"),
                '\x08' => out.push_str("\\b"),
                '\x0C' => out.push_str("\\f"),
                '\n' => out.push_str("\\n"),
                '\r' => out.push_str("\\r"),
                '\t' => out.push_str("\\t"),
                '\x0B' => out.push_str("\\v"),
                c if c.is_ascii_control() => out.push_str(&format!("\\x{:02x}", c as u32)),
                // What shows as nothing or as a blank is written out.
                c if c.is_control() || c != ' ' && c.is_whitespace() || is_format(c) => {
                    out.push_str(&format!("\\u{:04x}", c as u32));
                }
                c => out.push(c),
            }
        }
        for byte in chunk.invalid() {
            out.push_str(&format!("\\x{byte:02x}"));
        }
    }
    out.push('"');
    out
}

// ---- numbers

fn small(x: &BigUint) -> bool {
    (x.bits() as i64) < MAX_EXP
}

fn make_int(x: BigInt) -> Value {
    match i64::try_from(&x) {
        Ok(small) => Value::Int64(small),
        Err(_) => Value::Int(x),
    }
}

/// A fraction, or a float when its parts are too large to stay exact.
fn make_rat(x: Rat) -> Value {
    if small(x.num().magnitude()) && small(x.den()) {
        Value::Rat(x)
    } else {
        Value::Float(Float::from_ratio(x.num(), x.den(), PREC))
    }
}

/// A float; a result past the largest exponent is no value.
fn make_float(x: Option<Float>) -> Value {
    x.map_or(Value::Unknown, Value::Float)
}

fn make_complex(re: Value, im: Value) -> Value {
    if re.kind() == Kind::Unknown || im.kind() == Kind::Unknown {
        return Value::Unknown;
    }
    Value::Complex(Box::new(re), Box::new(im))
}

fn small_float(x: &Float) -> bool {
    let exp = x.mant_exp();
    -MAX_EXP < exp && exp < MAX_EXP
}

fn int_to_float(x: &BigInt) -> Float {
    Float::from_int(x, PREC)
}

fn rat_to_float(x: &Rat) -> Float {
    Float::from_ratio(x.num(), x.den(), PREC)
}

/// A number literal taken apart: `mant × base^-frac_digits × exp_base^exp`.
struct Number {
    base: u32,
    mant: BigUint,
    frac_digits: i64,
    exp: i64,
    /// 2 after `p`, 10 after `e` or without an exponent.
    exp_base: u32,
}

/// Reads a float or imaginary literal as Go's `big.Float` reads it: `0x`,
/// `0b` and `0o` give the base, a plain leading `0` does not.
fn scan(literal: &str) -> Option<Number> {
    let lower = literal.to_ascii_lowercase();
    let (base, rest) = match lower.get(..2) {
        Some("0x") => (16, &lower[2..]),
        Some("0b") => (2, &lower[2..]),
        Some("0o") => (8, &lower[2..]),
        _ => (10, lower.as_str()),
    };
    let exp_at = match base {
        16 => rest.find('p'),
        _ => rest.find(['e', 'p']),
    };
    let (mantissa, exponent) = match exp_at {
        Some(at) => (&rest[..at], Some(&rest[at..])),
        None => (rest, None),
    };
    let (whole, fraction) = mantissa.split_once('.').unwrap_or((mantissa, ""));
    let digits: String = whole
        .chars()
        .chain(fraction.chars())
        .filter(|&c| c != '_')
        .collect();
    let frac_digits = fraction.chars().filter(|&c| c != '_').count() as i64;
    let mant = match digits.as_str() {
        "" => return None,
        digits => BigUint::parse_bytes(digits.as_bytes(), base)?,
    };
    let (exp, exp_base) = match exponent {
        None => (0, 10),
        Some(exponent) => {
            let exp_base = if exponent.starts_with('p') { 2 } else { 10 };
            let digits: String = exponent[1..].chars().filter(|&c| c != '_').collect();
            (digits.parse::<i64>().ok()?, exp_base)
        }
    };
    Some(Number {
        base,
        mant,
        frac_digits,
        exp,
        exp_base,
    })
}

/// `5^n` as Go's `big.Float` computes it for a number of `prec` bits: by
/// squaring, each product rounded.
fn pow5(mut n: u64, prec: u32) -> Option<Float> {
    // The largest power of five that fits in 64 bits.
    const EXACT: u64 = 27;
    let five = |n: u32| Float::from_int(&BigInt::from(5u32).pow(n), u32::MAX);
    if n <= EXACT {
        return Some(five(n as u32));
    }
    let mut z = five(EXACT as u32);
    n -= EXACT;
    let mut f = five(1);
    while n > 0 {
        if n & 1 != 0 {
            z = z.mul(&f, prec)?;
        }
        n >>= 1;
        // The last square is not needed, and may be too large to hold.
        if n > 0 {
            f = f.mul(&f, prec + 64)?;
        }
    }
    Some(z)
}

impl Number {
    /// The powers of two and of five to multiply the mantissa by.
    fn exponents(&self) -> Option<(i64, i64)> {
        // A digit after the point divides by the base: by two and five
        // for a decimal digit, by a power of two otherwise.
        let (exp2, mut exp5) = match self.base {
            10 => (-self.frac_digits, -self.frac_digits),
            2 => (-self.frac_digits, 0),
            8 => (-self.frac_digits.checked_mul(3)?, 0),
            _ => (-self.frac_digits.checked_mul(4)?, 0),
        };
        if self.exp_base == 10 {
            exp5 = exp5.checked_add(self.exp)?;
        }
        Some((exp2.checked_add(self.exp)?, exp5))
    }

    /// The float of `PREC` bits that Go's `big.Float` makes of the
    /// literal: the power of five is itself rounded, so the result is
    /// close to the literal, not always the nearest float.
    fn to_float(&self) -> Option<Float> {
        if self.mant.bits() == 0 {
            return Some(Float::zero());
        }
        let (exp2, exp5) = self.exponents()?;
        let top = exp2.checked_add(self.mant.bits() as i64)?;
        if !(i64::from(i32::MIN)..=i64::from(i32::MAX)).contains(&top) {
            return None;
        }
        if exp5 == 0 {
            return Float::from_parts(self.mant.clone(), exp2, PREC);
        }
        let z = Float::from_parts(self.mant.clone(), exp2, u32::MAX)?;
        let power = pow5(exp5.unsigned_abs(), PREC + 64);
        match (exp5 < 0, power) {
            (true, Some(power)) => z.quo(&power, PREC),
            // Divided by a power too large to hold.
            (true, None) => Some(Float::zero()),
            (false, power) => z.mul(&power?, PREC),
        }
    }

    /// The exact value. The exponents are small.
    fn to_rat(&self) -> Option<Rat> {
        let (exp2, exp5) = self.exponents()?;
        let mut num = BigInt::from(self.mant.clone());
        let mut den = BigInt::from(1u32);
        let scale = |by: u32, exp: i64| BigInt::from(by).pow(exp.unsigned_abs() as u32);
        for (by, exp) in [(2, exp2), (5, exp5)] {
            if exp >= 0 {
                num *= scale(by, exp);
            } else {
                den *= scale(by, exp);
            }
        }
        Some(Rat::new(num, den))
    }
}

/// The value of a float literal: a fraction if its size allows.
fn float_from_literal(literal: &str) -> Value {
    let Some(number) = scan(literal) else {
        return Value::Unknown;
    };
    let Some(float) = number.to_float() else {
        return Value::Unknown;
    };
    // A literal too small for a float is zero; its exact value would be a
    // fraction of absurd size.
    if float.is_zero() {
        return Value::Rat(Rat::from_int(BigInt::from(0u32)));
    }
    if small_float(&float)
        && let Some(rat) = number.to_rat()
    {
        return Value::Rat(rat);
    }
    Value::Float(float)
}

/// The value of an integer literal: `0x`, `0o`, `0b`, or a leading `0`
/// for octal.
fn int_from_literal(literal: &str) -> Value {
    let digits: String = literal.chars().filter(|&c| c != '_').collect();
    let lower = digits.to_ascii_lowercase();
    let (base, digits) = match lower.get(..2) {
        Some("0x") => (16, &lower[2..]),
        Some("0b") => (2, &lower[2..]),
        Some("0o") => (8, &lower[2..]),
        _ if lower.len() > 1 && lower.starts_with('0') => (8, &lower[1..]),
        _ => (10, lower.as_str()),
    };
    let parsed = BigInt::parse_bytes(digits.as_bytes(), base);
    parsed.map_or(Value::Unknown, make_int)
}

impl Value {
    pub fn kind(&self) -> Kind {
        match self {
            Self::Unknown => Kind::Unknown,
            Self::Bool(_) => Kind::Bool,
            Self::Str(_) => Kind::String,
            Self::Int64(_) | Self::Int(_) => Kind::Int,
            Self::Rat(_) | Self::Float(_) => Kind::Float,
            Self::Complex(..) => Kind::Complex,
        }
    }

    pub fn string(bytes: &[u8]) -> Self {
        Self::Str(Str::new(bytes))
    }

    /// The value of the literal `text` of the kind `kind`; unknown if the
    /// literal is not well formed or its value is too large.
    pub fn from_literal(text: &str, kind: LitKind) -> Self {
        match kind {
            LitKind::Int => int_from_literal(text),
            LitKind::Float => float_from_literal(text),
            LitKind::Imag => match text.strip_suffix('i') {
                Some(number) => make_complex(Self::Int64(0), float_from_literal(number)),
                None => Self::Unknown,
            },
            LitKind::Rune => {
                let code = unquote_rune(text);
                code.map_or(Self::Unknown, |code| Self::Int64(i64::from(code)))
            }
            LitKind::String => {
                let bytes = unquote(text);
                bytes.map_or(Self::Unknown, |bytes| Self::string(&bytes))
            }
        }
    }

    /// The exact value of a `f64` that is a number.
    pub fn from_f64(x: f64) -> Self {
        if !x.is_finite() {
            return Self::Unknown;
        }
        // Without the sign of a negative zero. Every `f64` is small
        // enough for a fraction.
        Self::Rat(Rat::from_f64(x + 0.0))
    }

    /// -1, 0 or 1 for a number; 1 for an unknown value, so that it is not
    /// taken for a zero divisor.
    pub fn sign(&self) -> i32 {
        match self {
            Self::Int64(x) => x.signum() as i32,
            Self::Int(x) => match x.sign() {
                Sign::Minus => -1,
                Sign::NoSign => 0,
                Sign::Plus => 1,
            },
            Self::Rat(x) => x.sign(),
            Self::Float(x) => x.sign(),
            Self::Complex(re, im) => re.sign() | im.sign(),
            _ => 1,
        }
    }

    /// The number of bits of the magnitude of an integer.
    pub fn bit_len(&self) -> u64 {
        match self {
            Self::Int64(x) => u64::from(64 - x.unsigned_abs().leading_zeros()),
            Self::Int(x) => x.bits(),
            _ => 0,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Self::Int64(x) => Some(*x),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Self::Bool(x) => Some(*x),
            _ => None,
        }
    }

    fn as_big_int(&self) -> Option<BigInt> {
        match self {
            Self::Int64(x) => Some(BigInt::from(*x)),
            Self::Int(x) => Some(x.clone()),
            _ => None,
        }
    }

    // ---- conversions

    /// The integer that the number is, or unknown.
    pub fn to_int(&self) -> Self {
        match self {
            Self::Int64(_) | Self::Int(_) => self.clone(),
            Self::Rat(x) if x.is_int() => make_int(x.num().clone()),
            Self::Float(x) if small_float(x) => {
                // A float that is an integer but for the last few bits,
                // which earlier rounding lost, counts as that integer.
                const SLACK: u32 = 4;
                let modes = [
                    (PREC, Rounding::NearestEven),
                    (PREC - SLACK, Rounding::ToZero),
                    (PREC - SLACK, Rounding::AwayFromZero),
                ];
                for (prec, mode) in modes {
                    if let Some((int, true)) = x.round(prec, mode).map(|x| x.to_int()) {
                        return make_int(int);
                    }
                }
                Self::Unknown
            }
            Self::Complex(..) => match self.to_float() {
                float @ (Self::Rat(_) | Self::Float(_)) => float.to_int(),
                _ => Self::Unknown,
            },
            _ => Self::Unknown,
        }
    }

    /// The number as a float, or unknown.
    pub fn to_float(&self) -> Self {
        match self {
            Self::Int64(x) => Self::Rat(Rat::from_int(BigInt::from(*x))),
            Self::Int(x) if small(x.magnitude()) => Self::Rat(Rat::from_int(x.clone())),
            Self::Int(x) => Self::Float(int_to_float(x)),
            Self::Rat(_) | Self::Float(_) => self.clone(),
            Self::Complex(re, im) if im.sign() == 0 => re.to_float(),
            _ => Self::Unknown,
        }
    }

    pub fn to_complex(&self) -> Self {
        match self {
            Self::Int64(_) | Self::Int(_) | Self::Rat(_) | Self::Float(_) => {
                Self::Complex(Box::new(self.clone()), Box::new(Self::Int64(0)))
            }
            Self::Complex(..) => self.clone(),
            _ => Self::Unknown,
        }
    }

    pub fn real(&self) -> Self {
        match self {
            Self::Complex(re, _) => (**re).clone(),
            Self::Int64(_) | Self::Int(_) | Self::Rat(_) | Self::Float(_) => self.clone(),
            _ => Self::Unknown,
        }
    }

    pub fn imag(&self) -> Self {
        match self {
            Self::Complex(_, im) => (**im).clone(),
            Self::Int64(_) | Self::Int(_) | Self::Rat(_) | Self::Float(_) => Self::Int64(0),
            _ => Self::Unknown,
        }
    }

    /// `self × i` for a number that is not complex.
    pub fn make_imag(&self) -> Self {
        match self {
            Self::Int64(_) | Self::Int(_) | Self::Rat(_) | Self::Float(_) => {
                make_complex(Self::Int64(0), self.clone())
            }
            _ => Self::Unknown,
        }
    }

    /// The `f64` nearest to a number, in one rounding step; an infinity
    /// past the largest.
    pub fn to_f64(&self) -> f64 {
        self.to_ieee(BINARY64)
    }

    /// The `f32` nearest to a number, in one rounding step, as a `f64`.
    pub fn to_f32(&self) -> f64 {
        self.to_ieee(BINARY32)
    }

    fn to_ieee(&self, format: Ieee) -> f64 {
        match self {
            Self::Int64(x) => Rat::from_int(BigInt::from(*x)).to_ieee(format),
            // As Go: through a float of `PREC` bits, which may round first.
            Self::Int(x) => int_to_float(x).to_ieee(format),
            Self::Rat(x) => x.to_ieee(format),
            Self::Float(x) => x.to_ieee(format),
            _ => 0.0,
        }
    }

    // ---- operations

    /// `op self`. For `^x` of an unsigned type, `unsigned_bits` is its
    /// size; else zero. Unknown if the operand does not fit the operator.
    pub fn unary(&self, op: UnaryOp, unsigned_bits: u32) -> Self {
        match (op, self) {
            (_, Self::Unknown) => Self::Unknown,
            (UnaryOp::Plus, x) if !matches!(x, Self::Bool(_) | Self::Str(_)) => x.clone(),
            (UnaryOp::Neg, Self::Int64(x)) => match x.checked_neg() {
                Some(negated) => Self::Int64(negated),
                None => make_int(-BigInt::from(*x)),
            },
            (UnaryOp::Neg, Self::Int(x)) => make_int(-x),
            (UnaryOp::Neg, Self::Rat(x)) => make_rat(x.neg()),
            (UnaryOp::Neg, Self::Float(x)) => Self::Float(x.neg()),
            (UnaryOp::Neg, Self::Complex(re, im)) => make_complex(re.unary(op, 0), im.unary(op, 0)),
            (UnaryOp::Complement, Self::Int64(_) | Self::Int(_)) => {
                let Some(x) = self.as_big_int() else {
                    return Self::Unknown;
                };
                let mut complement = !x;
                if unsigned_bits > 0 {
                    complement &= (BigInt::from(1u32) << unsigned_bits) - 1u32;
                }
                make_int(complement)
            }
            (UnaryOp::Not, Self::Bool(x)) => Self::Bool(!x),
            _ => Self::Unknown,
        }
    }

    /// How far up the ladder of representations a value is.
    fn rank(&self) -> u8 {
        match self {
            Self::Unknown => 0,
            Self::Bool(_) | Self::Str(_) => 1,
            Self::Int64(_) => 2,
            Self::Int(_) => 3,
            Self::Rat(_) => 4,
            Self::Float(_) => 5,
            Self::Complex(..) => 6,
        }
    }

    /// `self` in the representation of `like`, which is higher up.
    fn raised(&self, like: &Self) -> Self {
        match (like, self) {
            (Self::Int(_), Self::Int64(x)) => Self::Int(BigInt::from(*x)),
            (Self::Rat(_), Self::Int64(x)) => Self::Rat(Rat::from_int(BigInt::from(*x))),
            (Self::Rat(_), Self::Int(x)) => Self::Rat(Rat::from_int(x.clone())),
            (Self::Float(_), Self::Int64(x)) => Self::Float(int_to_float(&BigInt::from(*x))),
            (Self::Float(_), Self::Int(x)) => Self::Float(int_to_float(x)),
            (Self::Float(_), Self::Rat(x)) => Self::Float(rat_to_float(x)),
            (Self::Complex(..), x) => x.to_complex(),
            // Nothing in common: the pair is unknown.
            _ => Self::Unknown,
        }
    }

    /// Both values in the representation of the one higher up.
    fn matched(&self, other: &Self) -> (Self, Self) {
        match self.rank().cmp(&other.rank()) {
            Ordering::Less => {
                let raised = self.raised(other);
                let other = if raised == Self::Unknown {
                    Self::Unknown
                } else {
                    other.clone()
                };
                (raised, other)
            }
            Ordering::Greater => {
                let (other, this) = other.matched(self);
                (this, other)
            }
            Ordering::Equal => (self.clone(), other.clone()),
        }
    }

    /// `self op other`. Unknown if an operand is, if the operands do not
    /// fit the operator, or for a division by zero.
    pub fn binary(&self, op: BinaryOp, other: &Self) -> Self {
        use BinaryOp::*;
        match self.matched(other) {
            (Self::Bool(x), Self::Bool(y)) => match op {
                LogicalAnd => Self::Bool(x && y),
                LogicalOr => Self::Bool(x || y),
                _ => Self::Unknown,
            },
            (Self::Int64(x), Self::Int64(y)) => {
                let fast = match op {
                    Add => x.checked_add(y),
                    Sub => x.checked_sub(y),
                    Mul => x.checked_mul(y),
                    IntQuo => x.checked_div(y),
                    Rem => x.checked_rem(y),
                    And => Some(x & y),
                    Or => Some(x | y),
                    Xor => Some(x ^ y),
                    AndNot => Some(x & !y),
                    Quo | LogicalAnd | LogicalOr => None,
                };
                match fast {
                    Some(z) => Self::Int64(z),
                    None => big_int_op(BigInt::from(x), op, BigInt::from(y)),
                }
            }
            (Self::Int(x), Self::Int(y)) => big_int_op(x, op, y),
            (Self::Rat(x), Self::Rat(y)) => match op {
                Add => make_rat(x.add(&y)),
                Sub => make_rat(x.sub(&y)),
                Mul => make_rat(x.mul(&y)),
                Quo if y.sign() != 0 => make_rat(x.quo(&y)),
                _ => Self::Unknown,
            },
            (Self::Float(x), Self::Float(y)) => match op {
                Add => make_float(x.add(&y, PREC)),
                Sub => make_float(x.sub(&y, PREC)),
                Mul => make_float(x.mul(&y, PREC)),
                Quo if !y.is_zero() => make_float(x.quo(&y, PREC)),
                _ => Self::Unknown,
            },
            (Self::Complex(a, b), Self::Complex(c, d)) => {
                let (re, im) = match op {
                    Add => (a.binary(Add, &c), b.binary(Add, &d)),
                    Sub => (a.binary(Sub, &c), b.binary(Sub, &d)),
                    Mul => {
                        let (ac, bd) = (a.binary(Mul, &c), b.binary(Mul, &d));
                        let (bc, ad) = (b.binary(Mul, &c), a.binary(Mul, &d));
                        (ac.binary(Sub, &bd), bc.binary(Add, &ad))
                    }
                    Quo => {
                        let (ac, bd) = (a.binary(Mul, &c), b.binary(Mul, &d));
                        let (bc, ad) = (b.binary(Mul, &c), a.binary(Mul, &d));
                        let size = c.binary(Mul, &c).binary(Add, &d.binary(Mul, &d));
                        let re = ac.binary(Add, &bd).binary(Quo, &size);
                        (re, bc.binary(Sub, &ad).binary(Quo, &size))
                    }
                    _ => return Self::Unknown,
                };
                make_complex(re, im)
            }
            (Self::Str(x), Self::Str(y)) if op == Add => Self::Str(x.join(&y)),
            _ => Self::Unknown,
        }
    }

    /// `self << count` or `self >> count` for an integer.
    pub fn shift(&self, left: bool, count: u32) -> Self {
        match (self.as_big_int(), left) {
            (Some(x), true) => make_int(x << count),
            // The shift of a negative number rounds down, as in Go.
            (Some(x), false) => make_int(x >> count),
            (None, _) => Self::Unknown,
        }
    }

    /// Whether `self op other`. False if an operand is unknown or the
    /// operands cannot be compared so.
    pub fn compare(&self, op: CompareOp, other: &Self) -> bool {
        let order = match self.matched(other) {
            (Self::Bool(x), Self::Bool(y)) => {
                return match op {
                    CompareOp::Eq => x == y,
                    CompareOp::Ne => x != y,
                    _ => false,
                };
            }
            (Self::Complex(a, b), Self::Complex(c, d)) => {
                let equal = a.compare(CompareOp::Eq, &c) && b.compare(CompareOp::Eq, &d);
                return match op {
                    CompareOp::Eq => equal,
                    CompareOp::Ne => !equal,
                    _ => false,
                };
            }
            (Self::Int64(x), Self::Int64(y)) => x.cmp(&y),
            (Self::Int(x), Self::Int(y)) => x.cmp(&y),
            (Self::Rat(x), Self::Rat(y)) => x.compare(&y),
            (Self::Float(x), Self::Float(y)) => x.compare(&y),
            (Self::Str(x), Self::Str(y)) => x.bytes().cmp(y.bytes()),
            _ => return false,
        };
        op.holds(order)
    }

    /// The value in full: an integer in decimal, a fraction as `a/b`, a
    /// float as its hexadecimal mantissa and exponent of two.
    pub fn exact_string(&self) -> String {
        match self {
            Self::Rat(x) if x.is_int() => x.num().to_string(),
            Self::Rat(x) => format!("{}/{}", x.num(), x.den()),
            Self::Float(x) => x.text_p(),
            Self::Complex(re, im) => {
                format!("({} + {}i)", re.exact_string(), im.exact_string())
            }
            Self::Str(x) => quoted(x.bytes()),
            _ => self.to_string(),
        }
    }
}

fn big_int_op(x: BigInt, op: BinaryOp, y: BigInt) -> Value {
    use BinaryOp::*;
    charge(x.bits() + y.bits());
    let zero_divisor = y.sign() == Sign::NoSign;
    match op {
        Add => make_int(x + y),
        Sub => make_int(x - y),
        Mul => make_int(x * y),
        Quo if !zero_divisor => make_rat(Rat::new(x, y)),
        IntQuo if !zero_divisor => make_int(x / y),
        Rem if !zero_divisor => make_int(x % y),
        And => make_int(x & y),
        Or => make_int(x | y),
        Xor => make_int(x ^ y),
        AndNot => make_int(x & !y),
        _ => Value::Unknown,
    }
}

// ---- text

/// `x` as C's `%g` writes it: with `digits` significant digits, or with as
/// few as tell it from every other `f64`.
fn format_g(x: f64, digits: Option<usize>) -> String {
    if x == 0.0 {
        return if x.is_sign_negative() { "-0" } else { "0" }.to_owned();
    }
    let scientific = match digits {
        Some(digits) => format!("{:.*e}", digits - 1, x),
        None => format!("{x:e}"),
    };
    // Rust always writes an exponent.
    let (mantissa, exp) = scientific.split_once('e').unwrap_or((&scientific, "0"));
    let exp: i32 = exp.parse().unwrap_or(0);
    let sign = if mantissa.starts_with('-') { "-" } else { "" };
    let all: String = mantissa.chars().filter(char::is_ascii_digit).collect();
    let digits = match all.trim_end_matches('0') {
        "" => "0",
        digits => digits,
    };
    if !(-4..6).contains(&exp) {
        let (first, rest) = digits.split_at(1);
        let point = if rest.is_empty() { "" } else { "." };
        let exp_sign = if exp < 0 { '-' } else { '+' };
        return format!("{sign}{first}{point}{rest}e{exp_sign}{:02}", exp.abs());
    }
    if exp < 0 {
        let zeros = "0".repeat((-exp - 1) as usize);
        return format!("{sign}0.{zeros}{digits}");
    }
    let whole = exp as usize + 1;
    if digits.len() <= whole {
        return format!("{sign}{digits}{}", "0".repeat(whole - digits.len()));
    }
    let (before, after) = digits.split_at(whole);
    format!("{sign}{before}.{after}")
}

/// `|x| × 10^by`, to more bits than a constant has.
fn scale10(x: &Float, by: i64) -> Option<Float> {
    let power = pow5(by.unsigned_abs(), PREC + 128)?;
    let scaled = if by >= 0 {
        x.mul(&power, PREC + 128)
    } else {
        x.quo(&power, PREC + 128)
    };
    scaled?.scale2(by)
}

/// A float outside the range of a `f64`, with six digits: `1.23457e+5000`.
fn huge_float_string(x: &Float) -> String {
    let (sign, magnitude) = if x.sign() < 0 {
        ("-", x.neg())
    } else {
        ("", x.clone())
    };
    // The exponent of ten, to within one.
    let mut exp = ((x.mant_exp() - 1) as f64 * std::f64::consts::LOG10_2).floor() as i64;
    let low = BigInt::from(100_000u32);
    let digits = loop {
        let Some(scaled) = scale10(&magnitude, 5 - exp) else {
            return format!("{sign}?e{exp:+}");
        };
        let digits = scaled.nearest_int();
        if digits < low {
            exp -= 1;
        } else if digits >= &low * 10u32 {
            exp += 1;
        } else {
            break digits.to_string();
        }
    };
    // Go finds the exponent of ten by cutting off `log10` of the power of
    // two, toward zero. Under one that can leave the first digit behind
    // the point: `0.894562e-4236`. Messages are to read as Go's do.
    let cut = (x.mant_exp() as f64 * std::f64::consts::LOG10_2).trunc() as i64;
    if x.mant_exp() < 0 && exp == cut - 2 {
        let digits = digits.trim_end_matches('0');
        return format!("{sign}0.{digits}e{:+}", exp + 1);
    }
    let (first, rest) = digits.split_at(1);
    let rest = rest.trim_end_matches('0');
    let point = if rest.is_empty() { "" } else { "." };
    format!("{sign}{first}{point}{rest}e{exp:+}")
}

/// A float for a message: about six digits.
fn float_string(x: &Float) -> String {
    let double = x.to_ieee(BINARY64);
    if (x.sign() == 0) == (double == 0.0) && double.is_finite() {
        let text = format_g(double, Some(6));
        if !x.is_int() && !text.contains('.') {
            return format_g(double, None);
        }
        return text;
    }
    huge_float_string(x)
}

impl fmt::Display for Value {
    /// The value for a message: a float with about six digits, a long
    /// string cut short.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Unknown => f.write_str("unknown"),
            Self::Bool(x) => write!(f, "{x}"),
            Self::Str(x) => {
                const MAX_CHARS: usize = 72;
                let quoted = quoted(x.bytes());
                if quoted.chars().count() <= MAX_CHARS {
                    return f.write_str(&quoted);
                }
                let cut: String = quoted.chars().take(MAX_CHARS - 3).collect();
                write!(f, "{cut}...")
            }
            Self::Int64(x) => write!(f, "{x}"),
            Self::Int(x) => write!(f, "{x}"),
            Self::Rat(x) => f.write_str(&float_string(&rat_to_float(x))),
            Self::Float(x) => f.write_str(&float_string(x)),
            Self::Complex(re, im) => write!(f, "({re} + {im}i)"),
        }
    }
}
