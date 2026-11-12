//! Fixed-seed mutants of the spec samples through the front-end targets.

use std::fs;

use gopp_fuzz::mutate::{Seed, mutate};
use gopp_fuzz::target;
use gopp_span::Dialect;
use gopp_testutil::samples;
use rand::rngs::ChaCha8Rng;
use rand::{RngExt, SeedableRng};

const MUTANTS_PER_SAMPLE: u64 = 60;

#[test]
fn mutated_samples_pass_the_front_end_oracles() {
    let files = samples::files();
    let seeds: Vec<_> = files
        .iter()
        .map(|path| {
            let dialect = Dialect::from_path(path).unwrap();
            let text = fs::read_to_string(path).unwrap();
            (dialect, Seed::new(text, dialect).unwrap())
        })
        .collect();
    for (index, (dialect, seed)) in seeds.iter().enumerate() {
        for round in 0..MUTANTS_PER_SAMPLE {
            let mut rng = ChaCha8Rng::seed_from_u64(index as u64 * MUTANTS_PER_SAMPLE + round);
            let donor = &seeds[rng.random_range(0..seeds.len())].1;
            let input = mutate(seed, donor, &mut rng);
            let verdict = match dialect {
                Dialect::Gopp => target::parse_gopp(&input),
                Dialect::Go => target::parse_go(&input),
            };
            let name = files[index].display();
            assert_eq!(verdict, Ok(()), "{name}, mutant {round}");
        }
    }
}

/// The seeds of the symbol target, with a byte changed, added or
/// dropped, and pieces of two put together.
#[test]
fn mutated_symbols_pass_the_symbol_oracles() {
    let seeds = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../tests/fuzz/seeds/symbol_name"
    );
    let mut files: Vec<_> = fs::read_dir(seeds)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .collect();
    files.sort();
    let symbols: Vec<Vec<u8>> = files.iter().map(|path| fs::read(path).unwrap()).collect();
    assert!(symbols.len() >= 27);
    let mut rng = ChaCha8Rng::seed_from_u64(1);
    for symbol in &symbols {
        // Each seed is a symbol.
        assert!(gopp_mangle::demangle(symbol, 4096).is_ok());
        assert_eq!(target::symbol_name(symbol), Ok(()));
        for round in 0..400 {
            let mut input = symbol.clone();
            let at = rng.random_range(0..input.len());
            match round % 4 {
                0 => input[at] = rng.random_range(b'0'..=b'z'),
                1 => input.insert(at, rng.random_range(b'0'..=b'z')),
                2 => drop(input.remove(at)),
                _ => {
                    let donor = &symbols[rng.random_range(0..symbols.len())];
                    let from = rng.random_range(0..donor.len());
                    input.splice(at.., donor[from..].iter().copied());
                }
            }
            let name = String::from_utf8_lossy(&input).into_owned();
            assert_eq!(target::symbol_name(&input), Ok(()), "{name}");
        }
    }
}
