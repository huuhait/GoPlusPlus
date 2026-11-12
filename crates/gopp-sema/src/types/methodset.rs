//! The methods of an interface by name.
//!
//! A set is a value: adding to it gives a new set that shares what did not
//! change. An interface that embeds another shares the other's methods
//! instead of copying them, so a chain of embedded interfaces takes time
//! and memory in proportion to its length, not to the sum of the sizes of
//! its method sets.

use std::sync::Arc;

use lasso::{Key as _, Spur};

use super::Method;

/// A name with the package it belongs to, which is none for an exported
/// name.
pub type Key = (Spur, Option<Spur>);

/// Methods with different names: a binary trie over the bits of a number
/// that each name has alone.
#[derive(Clone, Debug, Default)]
pub struct MethodSet(Link);

type Link = Option<Arc<Node>>;

#[derive(Debug)]
enum Node {
    Leaf {
        bits: u64,
        method: Method,
    },
    /// The methods whose next bit is 0, and those whose next bit is 1.
    /// There are at least two below.
    Branch {
        len: usize,
        zero: Link,
        one: Link,
    },
}

/// A number that no other name has, with its bits mixed so that names that
/// were interned one after the other part early in the trie.
fn bits((name, package): Key) -> u64 {
    let package = package.map_or(0, |package| package.into_usize() as u64 + 1);
    let mut bits = (name.into_usize() as u64) << 32 | package;
    // Each step can be undone, so different names keep different numbers.
    bits ^= bits >> 33;
    bits = bits.wrapping_mul(0xff51_afd7_ed55_8ccd);
    bits ^= bits >> 33;
    bits = bits.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
    bits ^ bits >> 33
}

fn same(a: &Link, b: &Link) -> bool {
    match (a, b) {
        (None, None) => true,
        (Some(a), Some(b)) => Arc::ptr_eq(a, b),
        _ => false,
    }
}

fn len(link: &Link) -> usize {
    match link.as_deref() {
        None => 0,
        Some(Node::Leaf { .. }) => 1,
        Some(Node::Branch { len, .. }) => *len,
    }
}

fn branch(zero: Link, one: Link) -> Link {
    let len = len(&zero) + len(&one);
    Some(Arc::new(Node::Branch { len, zero, one }))
}

/// The set of two leaves with different numbers, from the bit `depth` on.
fn pair(a: &Link, a_bits: u64, b: &Link, b_bits: u64, depth: u32) -> Link {
    match (a_bits >> depth & 1, b_bits >> depth & 1) {
        (0, 1) => branch(a.clone(), b.clone()),
        (1, 0) => branch(b.clone(), a.clone()),
        (0, _) => branch(pair(a, a_bits, b, b_bits, depth + 1), None),
        _ => branch(None, pair(a, a_bits, b, b_bits, depth + 1)),
    }
}

/// The methods of `a` and of `b`, which are sets from the bit `depth` on.
/// For a name that both have, `both` gets the two methods and the set
/// keeps that of `a`. `steps` counts the work.
fn union(
    a: &Link,
    b: &Link,
    depth: u32,
    both: &mut impl FnMut(&Method, &Method),
    steps: &mut u64,
) -> Link {
    *steps += 1;
    let (Some(x), Some(y)) = (a, b) else {
        return a.clone().or_else(|| b.clone());
    };
    if Arc::ptr_eq(x, y) {
        return a.clone();
    }
    // What did not change is shared, so that it is known as the same set
    // when it is met again.
    let rebuilt = |zero: Link, one: Link, of: &Link, old: (&Link, &Link)| match same(&zero, old.0)
        && same(&one, old.1)
    {
        true => of.clone(),
        false => branch(zero, one),
    };
    match (&**x, &**y) {
        (
            Node::Leaf { bits, method },
            Node::Leaf {
                bits: other_bits,
                method: other,
            },
        ) => {
            if bits != other_bits {
                return pair(a, *bits, b, *other_bits, depth);
            }
            both(method, other);
            a.clone()
        }
        (Node::Branch { zero, one, .. }, Node::Leaf { bits, .. }) => match bits >> depth & 1 {
            0 => {
                let merged = union(zero, b, depth + 1, both, steps);
                rebuilt(merged, one.clone(), a, (zero, one))
            }
            _ => {
                let merged = union(one, b, depth + 1, both, steps);
                rebuilt(zero.clone(), merged, a, (zero, one))
            }
        },
        (Node::Leaf { bits, .. }, Node::Branch { zero, one, .. }) => match bits >> depth & 1 {
            0 => {
                let merged = union(a, zero, depth + 1, both, steps);
                rebuilt(merged, one.clone(), b, (zero, one))
            }
            _ => {
                let merged = union(a, one, depth + 1, both, steps);
                rebuilt(zero.clone(), merged, b, (zero, one))
            }
        },
        (
            Node::Branch { zero, one, .. },
            Node::Branch {
                zero: other_zero,
                one: other_one,
                ..
            },
        ) => {
            let merged_zero = union(zero, other_zero, depth + 1, both, steps);
            let merged_one = union(one, other_one, depth + 1, both, steps);
            if same(&merged_zero, other_zero) && same(&merged_one, other_one) {
                return b.clone();
            }
            rebuilt(merged_zero, merged_one, a, (zero, one))
        }
    }
}

impl MethodSet {
    pub fn len(&self) -> usize {
        len(&self.0)
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_none()
    }

    /// The method of the name.
    pub fn get(&self, key: Key) -> Option<&Method> {
        let wanted = bits(key);
        let mut current = &self.0;
        let mut depth = 0;
        loop {
            match current.as_deref()? {
                Node::Leaf { bits, method } => return (*bits == wanted).then_some(method),
                Node::Branch { zero, one, .. } => {
                    current = match wanted >> depth & 1 {
                        0 => zero,
                        _ => one,
                    };
                    depth += 1;
                }
            }
        }
    }

    /// The methods of this set and of `other`. For a name that both
    /// have, `both` gets the method of this set and that of `other`, and
    /// the result keeps the first. `steps` counts the work.
    pub fn union(
        &self,
        other: &MethodSet,
        both: &mut impl FnMut(&Method, &Method),
        steps: &mut u64,
    ) -> MethodSet {
        MethodSet(union(&self.0, &other.0, 0, both, steps))
    }

    /// This set with `method`, as `union` has it.
    pub fn with(
        &self,
        method: Method,
        both: &mut impl FnMut(&Method, &Method),
        steps: &mut u64,
    ) -> MethodSet {
        let bits = bits((method.name, method.package));
        let leaf = MethodSet(Some(Arc::new(Node::Leaf { bits, method })));
        self.union(&leaf, both, steps)
    }

    /// Gives `each` every method, in no order that means anything.
    pub fn for_each(&self, each: &mut impl FnMut(&Method)) {
        let mut work = vec![&self.0];
        while let Some(link) = work.pop() {
            match link.as_deref() {
                None => {}
                Some(Node::Leaf { method, .. }) => each(method),
                Some(Node::Branch { zero, one, .. }) => work.extend([zero, one]),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use lasso::ThreadedRodeo;

    use super::*;
    use crate::types::Types;

    #[test]
    fn a_set_has_each_name_once_and_shares_what_it_embeds() {
        let names = ThreadedRodeo::new();
        let types = Types::new(&names);
        let method = |name: &str, results: &[_]| Method {
            name: names.get_or_intern(name),
            package: None,
            sig: types.func(&[], results, false),
        };
        let int = types.basic(crate::basic::Basic::Int);
        let add = |set: &MethodSet, method: Method, again: &mut Vec<String>| {
            let mut both = |m: &Method, _: &Method| again.push(names.resolve(&m.name).to_owned());
            set.with(method, &mut both, &mut 0)
        };
        let mut again = Vec::new();
        let mut small = MethodSet::default();
        for i in 0..100 {
            small = add(&small, method(&format!("m{i}"), &[]), &mut again);
        }
        assert_eq!((small.len(), again.len()), (100, 0));
        // A name that comes again is reported, and the first method stays.
        let same = add(&small, method("m7", &[int]), &mut again);
        assert_eq!(again, ["m7"]);
        let m7 = (names.get_or_intern("m7"), None);
        assert_eq!(same.get(m7).map(|m| m.sig), small.get(m7).map(|m| m.sig));
        assert!(small.get((names.get_or_intern("m100"), None)).is_none());

        // The union with a set that has one more method takes few steps:
        // what both have is one set.
        let larger = add(&small, method("extra", &[]), &mut again);
        let mut steps = 0;
        let union = small.union(&larger, &mut |_, _| {}, &mut steps);
        assert_eq!(union.len(), 101);
        assert!(steps < 40, "{steps} steps");

        let mut all = Vec::new();
        union.for_each(&mut |m| all.push(names.resolve(&m.name).to_owned()));
        all.sort();
        let mut expected: Vec<String> = (0..100).map(|i| format!("m{i}")).collect();
        expected.push("extra".to_owned());
        expected.sort();
        assert_eq!(all, expected);
    }
}
