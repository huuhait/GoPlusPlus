//! Fixed-seed mutants of the spec samples through the front-end targets.

use std::fs;

use gopp_fuzz::mutate::{Seed, mutate};
use gopp_fuzz::target;
use gopp_span::Dialect;
use gopp_testutil::samples;
use rand::rngs::ChaCha8Rng;
use rand::{RngExt, SeedableRng};

const MUTANTS_PER_SAMPLE: u64 = 100;

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
