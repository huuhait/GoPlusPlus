//! Step-count checks that an analysis scales near-linearly.

use std::time::{Duration, Instant};

/// How much faster than the input the step count may grow: 4x input, 4.5x steps.
const SLACK: f64 = 1.125;
const MAX_ROUNDS: usize = 8;
const WALL_LIMIT: Duration = Duration::from_secs(30);

/// Why a step function is not near-linear.
#[derive(Debug, PartialEq)]
pub struct Superlinear {
    pub smaller: (usize, u64),
    pub larger: (usize, u64),
}

/// Checks that `steps(size)` grows at most `SLACK` times faster than `size`
/// between consecutive sizes.
///
/// # Panics
///
/// Panics when the sizes cannot tell linear from quadratic growth (each must
/// be at least twice the one before), or when the run exceeds its round or
/// wall-time bound.
pub fn near_linear(sizes: &[usize], steps: impl Fn(usize) -> u64) -> Result<(), Superlinear> {
    assert!(
        (2..=MAX_ROUNDS).contains(&sizes.len()),
        "need 2 to {MAX_ROUNDS} sizes, got {}",
        sizes.len()
    );
    let started = Instant::now();
    let mut smaller = (sizes[0], steps(sizes[0]));
    for &size in &sizes[1..] {
        assert!(
            size >= 2 * smaller.0 && smaller.0 > 0,
            "size {size} must be at least twice {}",
            smaller.0
        );
        assert!(
            started.elapsed() < WALL_LIMIT,
            "stress run passed {WALL_LIMIT:?} before size {size}"
        );
        let larger = (size, steps(size));
        let allowed = smaller.1 as f64 * (size as f64 / smaller.0 as f64) * SLACK;
        if larger.1 as f64 > allowed {
            return Err(Superlinear { smaller, larger });
        }
        smaller = larger;
    }
    Ok(())
}

/// Panics unless [`near_linear`] holds.
pub fn assert_near_linear(sizes: &[usize], steps: impl Fn(usize) -> u64) {
    if let Err(Superlinear { smaller, larger }) = near_linear(sizes, steps) {
        panic!(
            "not near-linear: {} steps at size {}, {} steps at size {}",
            smaller.1, smaller.0, larger.1, larger.0
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SIZES: [usize; 4] = [1_000, 4_000, 16_000, 64_000];

    #[test]
    fn linear_steps_with_a_constant_cost_pass() {
        assert_near_linear(&SIZES, |n| 500 + 7 * n as u64);
    }

    #[test]
    fn quadratic_steps_fail() {
        let quadratic = |n: usize| (n * n) as u64;
        assert_eq!(
            near_linear(&SIZES, quadratic),
            Err(Superlinear {
                smaller: (1_000, 1_000_000),
                larger: (4_000, 16_000_000),
            })
        );
    }

    #[test]
    fn quadratic_steps_fail_at_the_smallest_allowed_gap() {
        assert!(near_linear(&[1_000, 2_000], |n| (n * n) as u64).is_err());
    }

    #[test]
    fn a_late_quadratic_term_fails() {
        let steps = |n: usize| (100 * n + n * n / 1_000) as u64;
        assert!(near_linear(&SIZES, steps).is_err());
    }

    #[test]
    #[should_panic(expected = "at least twice")]
    fn sizes_too_close_to_tell_are_refused() {
        let _ = near_linear(&[1_000, 1_100], |n| n as u64);
    }

    #[test]
    #[should_panic(expected = "not near-linear")]
    fn the_assertion_panics_on_quadratic_steps() {
        assert_near_linear(&SIZES, |n| (n * n) as u64);
    }
}
