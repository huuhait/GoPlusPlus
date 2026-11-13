//! What generic code keeps on the heap of its type parameters: no type
//! argument for such a parameter holds a pointer (OWN-B5). A body says
//! what it keeps, so the type arguments are held to it after every
//! body is checked.

use std::sync::Arc;

use super::*;
use crate::types::Held;

#[derive(Clone, Default)]
pub(super) struct Kept {
    /// The type parameters whose values a declaration puts on the heap,
    /// each with where and into what, in the order of the source.
    seeds: Vec<(TypeId, Span, Arc<str>)>,
    /// A type parameter, and a type parameter that a type argument for
    /// it holds a value of: what keeps the first keeps the second.
    flows: Vec<(TypeId, TypeId)>,
    /// The type arguments that hold a pointer, each with its type
    /// parameter and where it is written.
    given: Vec<(TypeId, TypeId, Span)>,
}

impl<'a> Checker<'a> {
    /// A value of each of the type parameters `params` is put on the
    /// heap at `span`, into `what`.
    pub(super) fn kept_where(&mut self, params: &[TypeId], span: Span, what: &str) {
        if let Some(aside) = &mut self.aside {
            let event = mono::Event::Kept(params.to_vec(), span, what.to_owned());
            return aside.mono.push(event);
        }
        let what: Arc<str> = what.into();
        for &param in params {
            self.kept.seeds.push((param, span, what.clone()));
        }
    }

    /// The type parameters `params` are given the type arguments `args`,
    /// each written at its span.
    pub(super) fn kept_given(
        &mut self,
        params: &[TypeId],
        args: &[TypeId],
        spans: &[Span],
        at: Span,
    ) {
        for (position, (&param, &arg)) in params.iter().zip(args).enumerate() {
            self.steps += 1;
            match self.types.held(arg) {
                Held::Pointer => {
                    let span = spans.get(position).copied().unwrap_or(at);
                    self.kept.given.push((param, arg, span));
                }
                Held::Params(held) => {
                    let flows = held.iter().map(|&held| (param, held));
                    self.kept.flows.extend(flows);
                }
            }
        }
    }

    /// The type parameter that a declaration writes for `param`: that
    /// of the type for one that a receiver names.
    fn kept_param(&self, param: TypeId) -> TypeId {
        let param = self.types.param_origin(param);
        self.mono_canon_of(param)
    }

    /// Follows what is kept from the type parameters that a body keeps
    /// to those that are given to them, says so for the packages that
    /// import this one, and reports each type argument that holds a
    /// pointer and is kept. Everything is taken in the order of the
    /// source, so the same is said however the bodies were checked.
    pub(super) fn kept_report(&mut self) {
        let kept = std::mem::take(&mut self.kept);
        let mut flows: FxHashMap<TypeId, Vec<TypeId>> = FxHashMap::default();
        let mut rest = Vec::new();
        let seeds = kept.seeds.iter().map(|(param, _, what)| (*param, what));
        for (param, what) in seeds {
            let param = self.kept_param(param);
            if self.types.kept(param).is_none() {
                self.types.set_kept(param, what.clone());
                rest.push(param);
            }
        }
        // A type parameter of another package is known to be kept.
        for &(param, given) in &kept.flows {
            self.steps += 1;
            let (param, given) = (self.kept_param(param), self.kept_param(given));
            let known = flows.entry(param).or_default();
            if known.is_empty() && self.types.kept(param).is_some() {
                rest.push(param);
            }
            known.push(given);
        }
        while let Some(param) = rest.pop() {
            self.steps += 1;
            let Some(what) = self.types.kept(param) else {
                continue;
            };
            for &given in flows.get(&param).into_iter().flatten() {
                if self.types.kept(given).is_none() {
                    self.types.set_kept(given, what.clone());
                    rest.push(given);
                }
            }
        }
        for (param, arg, span) in kept.given {
            let Some(what) = self.types.kept(self.kept_param(param)) else {
                continue;
            };
            let message = format!(
                "cannot use {} for {}: a value of it is kept in {what}, and a pointer in it may \
                 outlive what it points to",
                self.type_string(arg),
                self.type_string(param)
            );
            let fix = (Code::NoteOwnFixHeapPointer, own::HEAP_FIX.to_owned());
            self.error_with_fix(Code::OwnPointerOnHeap, span, message, fix);
        }
    }
}
