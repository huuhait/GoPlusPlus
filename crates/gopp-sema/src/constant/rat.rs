//! An exact fraction of two integers.

use std::cmp::Ordering;

use num_bigint::{BigInt, BigUint, Sign};

use super::float::{Ieee, ieee};

/// `num / den` in lowest terms, with `den > 0`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Rat {
    num: BigInt,
    den: BigUint,
}

fn gcd(mut a: BigUint, mut b: BigUint) -> BigUint {
    while b.bits() != 0 {
        (a, b) = (b.clone(), a % b);
    }
    a
}

impl Rat {
    /// `num / den`; `den` is not zero.
    pub fn new(num: BigInt, den: BigInt) -> Self {
        let num = if den.sign() == Sign::Minus { -num } else { num };
        Self::reduced(num, den.into_parts().1)
    }

    fn reduced(num: BigInt, den: BigUint) -> Self {
        super::charge(num.bits() + den.bits());
        let common = gcd(num.magnitude().clone(), den.clone());
        if common.bits() <= 1 {
            return Self { num, den };
        }
        Self {
            num: num / BigInt::from(common.clone()),
            den: den / common,
        }
    }

    pub fn from_int(x: BigInt) -> Self {
        Self {
            num: x,
            den: BigUint::from(1u32),
        }
    }

    pub fn num(&self) -> &BigInt {
        &self.num
    }

    pub fn den(&self) -> &BigUint {
        &self.den
    }

    pub fn is_int(&self) -> bool {
        self.den.bits() == 1
    }

    pub fn sign(&self) -> i32 {
        match self.num.sign() {
            Sign::Minus => -1,
            Sign::NoSign => 0,
            Sign::Plus => 1,
        }
    }

    pub fn neg(&self) -> Self {
        Self {
            num: -&self.num,
            den: self.den.clone(),
        }
    }

    fn den_int(&self) -> BigInt {
        BigInt::from(self.den.clone())
    }

    pub fn add(&self, other: &Self) -> Self {
        let num = &self.num * other.den_int() + &other.num * self.den_int();
        Self::reduced(num, &self.den * &other.den)
    }

    pub fn sub(&self, other: &Self) -> Self {
        self.add(&other.neg())
    }

    pub fn mul(&self, other: &Self) -> Self {
        Self::reduced(&self.num * &other.num, &self.den * &other.den)
    }

    /// `self / other`; `other` is not zero.
    pub fn quo(&self, other: &Self) -> Self {
        Self::new(&self.num * other.den_int(), &other.num * self.den_int())
    }

    pub fn compare(&self, other: &Self) -> Ordering {
        (&self.num * other.den_int()).cmp(&(&other.num * self.den_int()))
    }

    /// The nearest number of `format`, as a `f64`.
    pub fn to_ieee(&self, format: Ieee) -> f64 {
        // Enough bits of the quotient for the rounding bit and one more.
        let wanted = u64::from(format.mantissa_bits) + 2 + self.den.bits();
        let shift = wanted.saturating_sub(self.num.bits());
        let dividend = self.num.magnitude() << shift;
        let quotient = &dividend / &self.den;
        let sticky = &quotient * &self.den != dividend;
        let neg = self.num.sign() == Sign::Minus;
        ieee(neg, &quotient, -(shift as i64), sticky, format)
    }

    /// The exact value of a finite `f64`.
    pub fn from_f64(x: f64) -> Self {
        let bits = x.to_bits();
        let fraction = bits & ((1 << 52) - 1);
        let biased = (bits >> 52) & 0x7ff;
        // A subnormal number has no hidden bit and the smallest exponent.
        let (mantissa, exp) = match biased {
            0 => (fraction, -1074),
            _ => (fraction | 1 << 52, biased as i64 - 1075),
        };
        let sign = if x.is_sign_negative() {
            Sign::Minus
        } else {
            Sign::Plus
        };
        let mantissa = BigInt::from_biguint(sign, BigUint::from(mantissa));
        if exp >= 0 {
            Self::from_int(mantissa << exp as u64)
        } else {
            Self::reduced(mantissa, BigUint::from(1u32) << exp.unsigned_abs())
        }
    }
}
