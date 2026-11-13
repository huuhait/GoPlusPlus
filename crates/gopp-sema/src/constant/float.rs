//! A binary floating-point number with a mantissa of any length.
//!
//! An operation gives its exact result rounded to the precision asked for,
//! to the nearest value, ties to even. A result past the largest exponent is no number (`None`); one
//! under the smallest is zero.

use std::cmp::Ordering;

use num_bigint::{BigInt, BigUint, Sign};

/// The largest and the smallest exponent of two.
const MAX_EXP: i64 = i32::MAX as i64;
const MIN_EXP: i64 = i32::MIN as i64;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Rounding {
    NearestEven,
    ToZero,
    AwayFromZero,
}

/// `±mant × 2^exp`. The mantissa is odd, or zero for the value zero.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Float {
    neg: bool,
    mant: BigUint,
    exp: i64,
}

/// A format of IEEE 754: the bits of its mantissa, the exponent of the
/// lowest bit of its smallest number, and its largest exponent.
#[derive(Clone, Copy)]
pub struct Ieee {
    pub mantissa_bits: u32,
    pub min_exp: i64,
    pub max_exp: i64,
}

pub const BINARY64: Ieee = Ieee {
    mantissa_bits: 53,
    min_exp: -1074,
    max_exp: 1023,
};

pub const BINARY32: Ieee = Ieee {
    mantissa_bits: 24,
    min_exp: -149,
    max_exp: 127,
};

/// `mant` without its lowest `shift` bits, rounded. `sticky` says that the
/// exact value had more below `mant`.
fn shift_rounded(mant: &BigUint, shift: u64, sticky: bool, mode: Rounding) -> BigUint {
    if shift == 0 {
        return mant.clone();
    }
    let kept = mant >> shift;
    let lowest = mant.trailing_zeros().unwrap_or(u64::MAX);
    let half = mant.bit(shift - 1);
    let below_half = sticky || lowest < shift - 1;
    let up = match mode {
        Rounding::NearestEven => half && (below_half || kept.bit(0)),
        Rounding::ToZero => false,
        Rounding::AwayFromZero => half || below_half,
    };
    if up { kept + 1u32 } else { kept }
}

impl Float {
    pub fn zero() -> Self {
        Self {
            neg: false,
            mant: BigUint::default(),
            exp: 0,
        }
    }

    pub fn is_zero(&self) -> bool {
        self.mant.bits() == 0
    }

    /// -1, 0 or 1.
    pub fn sign(&self) -> i32 {
        match (self.is_zero(), self.neg) {
            (true, _) => 0,
            (false, true) => -1,
            (false, false) => 1,
        }
    }

    /// Whether the value is an integer.
    pub fn is_int(&self) -> bool {
        self.exp >= 0
    }

    /// The exponent `e` of `0.1… × 2^e`; zero for zero.
    pub fn mant_exp(&self) -> i64 {
        if self.is_zero() {
            0
        } else {
            self.exp + self.mant.bits() as i64
        }
    }

    /// `±mant × 2^exp` rounded to `prec` bits; `sticky` says that the exact
    /// value is a little more than that.
    fn rounded(
        neg: bool,
        mant: BigUint,
        exp: i64,
        sticky: bool,
        prec: u32,
        mode: Rounding,
    ) -> Option<Self> {
        let bits = mant.bits();
        if bits == 0 {
            return Some(Self::zero());
        }
        super::charge(bits);
        let shift = bits.saturating_sub(u64::from(prec));
        let mant = shift_rounded(&mant, shift, sticky, mode);
        let exp = exp + shift as i64;
        let zeros = mant.trailing_zeros().unwrap_or(0);
        let mant = mant >> zeros;
        let exp = exp + zeros as i64;
        let top = exp + mant.bits() as i64;
        if top > MAX_EXP {
            return None;
        }
        if top < MIN_EXP {
            return Some(Self::zero());
        }
        Some(Self { neg, mant, exp })
    }

    /// `self` rounded to `prec` bits.
    pub fn round(&self, prec: u32, mode: Rounding) -> Option<Self> {
        Self::rounded(self.neg, self.mant.clone(), self.exp, false, prec, mode)
    }

    /// `mant × 2^exp`, rounded to `prec` bits.
    pub fn from_parts(mant: BigUint, exp: i64, prec: u32) -> Option<Self> {
        Self::rounded(false, mant, exp, false, prec, Rounding::NearestEven)
    }

    pub fn from_int(x: &BigInt, prec: u32) -> Self {
        let neg = x.sign() == Sign::Minus;
        let rounded = Self::rounded(
            neg,
            x.magnitude().clone(),
            0,
            false,
            prec,
            Rounding::NearestEven,
        );
        // An integer that fits in memory is far below the largest exponent.
        rounded.unwrap_or_else(Self::zero)
    }

    /// `num / den`, rounded to `prec` bits. `den` is not zero.
    pub fn from_ratio(num: &BigInt, den: &BigUint, prec: u32) -> Self {
        let num = Self::from_int(num, u32::MAX);
        let den = Self::from_int(&BigInt::from(den.clone()), u32::MAX);
        num.quo(&den, prec).unwrap_or_else(Self::zero)
    }

    pub fn neg(&self) -> Self {
        Self {
            neg: !self.neg && !self.is_zero(),
            ..self.clone()
        }
    }

    pub fn add(&self, other: &Self, prec: u32) -> Option<Self> {
        if self.is_zero() {
            return other.round(prec, Rounding::NearestEven);
        }
        if other.is_zero() {
            return self.round(prec, Rounding::NearestEven);
        }
        let (mut high, mut low) = (self, other);
        if high.mant_exp() < low.mant_exp() {
            (high, low) = (low, high);
        }
        // The larger one gets room for every bit that can matter. If the
        // smaller one lies wholly below that, only its sign and the fact
        // that it is not zero reach the rounding: it is put at a quarter of
        // the lowest bit.
        let room = u64::from(prec) + 2;
        let mut high_mant = &high.mant << room;
        let mut exp = high.exp - room as i64;
        let low_mant = if low.mant_exp() <= exp {
            high_mant <<= 2u32;
            exp -= 2;
            BigUint::from(1u32)
        } else if low.exp >= exp {
            &low.mant << (low.exp - exp) as u64
        } else {
            high_mant <<= (exp - low.exp) as u64;
            exp = low.exp;
            low.mant.clone()
        };
        let (neg, mant) = if high.neg == low.neg {
            (high.neg, high_mant + low_mant)
        } else if high_mant >= low_mant {
            (high.neg, high_mant - low_mant)
        } else {
            (low.neg, low_mant - high_mant)
        };
        Self::rounded(neg, mant, exp, false, prec, Rounding::NearestEven)
    }

    pub fn sub(&self, other: &Self, prec: u32) -> Option<Self> {
        self.add(&other.neg(), prec)
    }

    pub fn mul(&self, other: &Self, prec: u32) -> Option<Self> {
        let neg = self.neg != other.neg;
        let mant = &self.mant * &other.mant;
        let exp = self.exp.checked_add(other.exp)?;
        Self::rounded(neg, mant, exp, false, prec, Rounding::NearestEven)
    }

    /// `self / other`; `other` is not zero.
    pub fn quo(&self, other: &Self, prec: u32) -> Option<Self> {
        if self.is_zero() {
            return Some(Self::zero());
        }
        // Enough bits of the quotient for the rounding bit and one more;
        // the rest of the division is a sticky bit.
        let wanted = u64::from(prec) + 2 + other.mant.bits();
        let shift = wanted.saturating_sub(self.mant.bits());
        let dividend = &self.mant << shift;
        let quotient = &dividend / &other.mant;
        let sticky = &quotient * &other.mant != dividend;
        let exp = self.exp.checked_sub(other.exp)?.checked_sub(shift as i64)?;
        let neg = self.neg != other.neg;
        Self::rounded(neg, quotient, exp, sticky, prec, Rounding::NearestEven)
    }

    pub fn compare(&self, other: &Self) -> Ordering {
        let by_sign = self.sign().cmp(&other.sign());
        if by_sign != Ordering::Equal || self.is_zero() {
            return by_sign;
        }
        let by_size = self.mant_exp().cmp(&other.mant_exp()).then_with(|| {
            // Equal top bits: the mantissas compare once their lowest bits
            // are at the same place.
            let low = self.exp.min(other.exp);
            let a = &self.mant << (self.exp - low) as u64;
            let b = &other.mant << (other.exp - low) as u64;
            a.cmp(&b)
        });
        if self.neg { by_size.reverse() } else { by_size }
    }

    /// The integer part, and whether `self` is that integer. The exponent
    /// of `self` is small enough to write the integer out.
    pub fn to_int(&self) -> (BigInt, bool) {
        let (magnitude, exact) = if self.exp >= 0 {
            (&self.mant << self.exp as u64, true)
        } else {
            // The mantissa is odd, so bits are lost.
            (&self.mant >> self.exp.unsigned_abs(), false)
        };
        let sign = if self.neg { Sign::Minus } else { Sign::Plus };
        (BigInt::from_biguint(sign, magnitude), exact)
    }

    /// The exact fraction `num / den`. The exponent of `self` is small
    /// enough to write it out.
    pub fn to_ratio(&self) -> (BigInt, BigUint) {
        let sign = if self.neg { Sign::Minus } else { Sign::Plus };
        let one = BigUint::from(1u32);
        if self.exp >= 0 {
            let num = &self.mant << self.exp as u64;
            (BigInt::from_biguint(sign, num), one)
        } else {
            let num = BigInt::from_biguint(sign, self.mant.clone());
            (num, one << self.exp.unsigned_abs())
        }
    }

    /// The nearest number of `format`, as a `f64`. Past the largest number
    /// it is an infinity.
    pub fn to_ieee(&self, format: Ieee) -> f64 {
        ieee(self.neg, &self.mant, self.exp, false, format)
    }

    /// `self × 2^by`.
    pub fn scale2(&self, by: i64) -> Option<Self> {
        let exp = self.exp.checked_add(by)?;
        Self::rounded(
            self.neg,
            self.mant.clone(),
            exp,
            false,
            u32::MAX,
            Rounding::NearestEven,
        )
    }

    /// The nearest integer, ties to even. The exponent of `self` is small
    /// enough to write the integer out.
    pub fn nearest_int(&self) -> BigInt {
        let whole_bits = self.mant_exp().max(0) as u32;
        // Rounded to its whole bits, the number is an integer; under one
        // half it is zero.
        let rounded = match whole_bits {
            0 if self.mant_exp() < 0 => None,
            bits => self.round(bits.max(1), Rounding::NearestEven),
        };
        rounded.map_or_else(BigInt::default, |rounded| rounded.to_int().0)
    }

    /// `-0x.8p+1`: the mantissa in hexadecimal after the point, then the
    /// exponent of two.
    pub fn text_p(&self) -> String {
        if self.is_zero() {
            return "0".to_owned();
        }
        let bits = self.mant.bits();
        let aligned = &self.mant << ((4 - bits % 4) % 4);
        let digits = aligned.to_str_radix(16);
        let digits = digits.trim_end_matches('0');
        let sign = if self.neg { "-" } else { "" };
        format!("{sign}0x.{digits}p{:+}", self.mant_exp())
    }
}

/// `±mant × 2^exp` (and a little more with `sticky`) as the nearest number
/// of `format`.
pub fn ieee(neg: bool, mant: &BigUint, exp: i64, sticky: bool, format: Ieee) -> f64 {
    let signed = |x: f64| if neg { -x } else { x };
    let bits = mant.bits() as i64;
    if bits == 0 {
        return signed(0.0);
    }
    let precision = i64::from(format.mantissa_bits);
    // Far outside the format, without shifting by a huge count.
    let top = exp.saturating_add(bits);
    if top > format.max_exp + 2 {
        return signed(f64::INFINITY);
    }
    if top < format.min_exp - 1 {
        return signed(0.0);
    }
    // The exponent of the lowest bit that the format keeps for this size.
    let mut lowest = (top - precision).max(format.min_exp);
    let mut kept = if lowest > exp {
        shift_rounded(mant, (lowest - exp) as u64, sticky, Rounding::NearestEven)
    } else {
        mant << (exp - lowest) as u64
    };
    // Rounding up may have carried into one more bit.
    if kept.bits() as i64 > precision {
        kept >>= 1u32;
        lowest += 1;
    }
    let Ok(kept) = u64::try_from(&kept) else {
        return signed(f64::INFINITY);
    };
    let normal = kept >> (precision - 1) != 0;
    if normal && lowest + precision - 1 > format.max_exp {
        return signed(f64::INFINITY);
    }
    // The value is `kept × 2^lowest`, exactly a `f64` whatever the format.
    let double = kept as f64;
    let scaled = if lowest >= -1022 {
        double * f64::from_bits(((lowest + 1023) as u64) << 52)
    } else {
        // Two steps keep the factors in range; the product is exact
        // because the format has no more bits than a `f64` down there.
        double * f64::from_bits(1 << 52) * f64::from_bits(((lowest + 1022 + 1023) as u64) << 52)
    };
    signed(scaled)
}
