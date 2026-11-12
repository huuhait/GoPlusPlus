//! The checks that take long: run with `--ignored`.

use std::time::{Duration, Instant};

use rand::rngs::ChaCha8Rng;
use rand::{RngExt, SeedableRng};

use crate::sema::{Entity, symbol_of};
use crate::sema_symbols::{World, checker_shapes};
use crate::symbol_name::{Cx, corpus, holds, mutant, shapes};
use crate::{Context, parse};

/// Whether the times of a small and a 16 times larger input fit
/// linear work, with room for the caches.
fn linear(small: Duration, large: Duration) -> bool {
    large <= small.max(Duration::from_micros(200)) * 16 * 6
}

fn timed(run: impl FnOnce()) -> Duration {
    let start = Instant::now();
    run();
    start.elapsed()
}

#[test]
#[ignore = "long"]
fn a_quadratic_walk_is_told_from_a_linear_one() {
    let walk = |size: u64| {
        let mut sum = 0u64;
        for a in 0..size {
            for b in 0..size {
                sum = sum.wrapping_add(std::hint::black_box(a ^ b));
            }
        }
        std::hint::black_box(sum);
    };
    assert!(!linear(timed(|| walk(4_000)), timed(|| walk(64_000))));
}

#[test]
#[ignore = "long"]
fn hostile_text_costs_linear_time() {
    for shape in 0..8 {
        let time = |size: usize| {
            let mut cx = Cx::default();
            let entity = shapes(&mut cx, size)[shape];
            let name = cx.symbol(entity);
            timed(|| {
                let written = cx.symbol(entity);
                let mut other = Context::new();
                let parsed = parse(&mut other, name.as_bytes()).unwrap();
                assert_eq!(written, name);
                other.readable(parsed, 1 << 20);
            })
        };
        let (small, large) = (time(16_000), time(256_000));
        assert!(
            linear(small, large),
            "shape {shape}: {small:?} and {large:?}"
        );
    }
}

#[test]
#[ignore = "long"]
fn symbol_of_costs_linear_time() {
    for shape in 0..5 {
        let time = |size: usize| {
            let world = World::new();
            let ty = checker_shapes(&world, size)[shape];
            let mut namer = world.namer();
            timed(|| {
                symbol_of(&mut namer, Entity::TypeInfo(ty)).unwrap();
            })
        };
        let (small, large) = (time(10_000), time(160_000));
        assert!(
            linear(small, large),
            "shape {shape}: {small:?} and {large:?}"
        );
    }
}

#[test]
#[ignore = "long"]
fn a_name_a_million_levels_deep_round_trips() {
    let mut cx = Cx::default();
    for entity in shapes(&mut cx, 1_000_000) {
        let name = cx.symbol(entity);
        let mut other = Context::new();
        let parsed = parse(&mut other, name.as_bytes()).unwrap();
        assert_eq!(other.symbol(parsed), name);
        assert_eq!(
            other.readable(parsed, 1 << 20),
            cx.0.readable(entity, 1 << 20)
        );
    }
}

#[test]
#[ignore = "long"]
fn a_million_and_a_half_mutated_names() {
    let mut cx = Cx::default();
    let names: Vec<String> = corpus(&mut cx, 3000, 11)
        .into_iter()
        .map(|entity| cx.symbol(entity))
        .collect();
    let mut rng = ChaCha8Rng::seed_from_u64(12);
    let mut taken = 0;
    for round in 0..1_500_000 {
        let name = names[round % names.len()].as_bytes();
        let donor = names[rng.random_range(0..names.len())].as_bytes();
        taken += usize::from(holds(&mutant(name, donor, &mut rng)));
    }
    assert!(taken > 15_000, "{taken}");
}
