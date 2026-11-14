//! The record of a generic body for one list of type arguments.

use gopp_syntax::ast::ExprId;
use lasso::{Spur, ThreadedRodeo};
use rustc_hash::FxHashMap;

use crate::foreign::TemplateArg;
use crate::object::{
    Body, ExprType, ForeignUse, Instance, Local, LocalKind, OperatorUse, Selection,
};
use crate::types::{IntoInterface, Marks, Member, TypeId, Types};

/// Where a generic body is written.
#[derive(Clone, Copy)]
pub struct Written<'a> {
    pub types: &'a Types,
    pub names: &'a ThreadedRodeo,
    /// The path of its package.
    pub package: Spur,
}

impl Body {
    /// What the checker recorded for a generic body, read with the type
    /// arguments `to` in the place of its type parameters: every type
    /// is the one that the arguments make it, and what a type argument
    /// decides is decided as for a body that is written with it.
    pub fn instantiated(&self, written: Written<'_>, to: &[TypeId]) -> Body {
        let Written {
            types,
            names,
            package,
        } = written;
        let from = &self.type_params[..];
        // A type that the body declares is a type of its own in each
        // instance: a new type that is declared as the type of the
        // generic body is, read with the type arguments.
        let mut replacing = types.replacing(from, to);
        let mut declared = Vec::new();
        for local in &self.locals {
            let named = match local.kind {
                LocalKind::Type => types.local_type(local.ty),
                _ => None,
            };
            if let Some(named) = named {
                declared.push((named, types.local_copy(named)));
            }
        }
        for &(old, new) in &declared {
            replacing.rename(old, new);
        }
        for &(old, new) in &declared {
            if let Some(declared_as) = types.declared_as(old) {
                types.set_underlying(new, replacing.ty(declared_as));
            }
        }
        // A type stands in many places of a body, and is worked out once.
        let mut made: FxHashMap<TypeId, TypeId> = FxHashMap::default();
        let mut ty = |ty: TypeId| *made.entry(ty).or_insert_with(|| replacing.ty(ty));
        let locals = (self.locals.iter())
            .map(|local| Local {
                name: local.name,
                kind: local.kind,
                span: local.span,
                used: local.used,
                ty: ty(local.ty),
            })
            .collect();
        let expr_types = (self.types.iter())
            .map(|(&expr, typed)| {
                let typed = ExprType {
                    ty: ty(typed.ty),
                    value: typed.value.clone(),
                };
                (expr, typed)
            })
            .collect();
        // A method of a type parameter is the method that the type
        // argument has by its name, which may be one of a type that it
        // embeds.
        let mut pointer_receivers = Vec::new();
        let selections: FxHashMap<_, _> = (self.selections.iter())
            .map(|(&expr, selection)| {
                let (recv, holder) = (ty(selection.recv), ty(selection.holder));
                let found = (self.param_methods.get(&expr))
                    .and_then(|&name| types.lookup(holder, name, package, names, false).ok());
                let selection = match found {
                    Some(found) => {
                        if let Member::Method {
                            pointer_receiver: true,
                            ..
                        } = found.member
                        {
                            pointer_receivers.push(expr);
                        }
                        Selection {
                            kind: selection.kind,
                            recv,
                            holder: found.holder,
                            path: found.path,
                            indirect: found.indirect || selection.indirect,
                        }
                    }
                    None => Selection {
                        recv,
                        holder,
                        ..selection.clone()
                    },
                };
                (expr, selection)
            })
            .collect();
        let instances = (self.instances.iter())
            .map(|(&expr, instance)| {
                let instance = Instance {
                    args: instance.args.iter().map(|&arg| ty(arg)).collect(),
                    ty: ty(instance.ty),
                };
                (expr, instance)
            })
            .collect();
        let mut operator = |used: &OperatorUse| OperatorUse {
            holder: ty(used.holder),
            sig: ty(used.sig),
            ..used.clone()
        };
        let mut operators = |all: &FxHashMap<_, OperatorUse>| {
            (all.iter())
                .map(|(&expr, used)| (expr, operator(used)))
                .collect()
        };
        let (op_uses, op_assigns) = (operators(&self.operators), operators(&self.op_assigns));
        let (index_stores, case_operators) = (
            operators(&self.index_stores),
            operators(&self.case_operators),
        );
        let range_operators = operators(&self.range_operators);
        // A value of a type parameter that is put into an interface is
        // an interface value itself if the type argument is one: it
        // goes on as it is, or says its type another way.
        let mut iface_conversions = FxHashMap::default();
        let mut iface_changes = FxHashMap::default();
        let both = self.iface_conversions.iter().chain(&self.iface_changes);
        for (&expr, &(from, to)) in both {
            match types.into_interface(ty(from), ty(to), names) {
                Some((IntoInterface::Put, from, to)) => iface_conversions.insert(expr, (from, to)),
                Some((IntoInterface::Changed, from, to)) => iface_changes.insert(expr, (from, to)),
                None => None,
            };
        }
        let part_conversions = (self.part_conversions.iter())
            .filter_map(|(&part, &(from, to))| {
                let (_, from, to) = types.into_interface(ty(from), ty(to), names)?;
                Some((part, (from, to)))
            })
            .collect();
        let case_types = (self.case_types.iter())
            .map(|(&expr, &case)| (expr, ty(case)))
            .collect();
        let foreign_uses = (self.foreign_uses.iter())
            .map(|(&expr, used)| {
                let explicit = (used.explicit.iter())
                    .map(|arg| match arg {
                        TemplateArg::Type(arg) => TemplateArg::Type(ty(*arg)),
                        TemplateArg::Const(value, of) => TemplateArg::Const(value.clone(), ty(*of)),
                    })
                    .collect();
                let used = ForeignUse {
                    import: used.import,
                    name: used.name,
                    explicit,
                    ty: ty(used.ty),
                };
                (expr, used)
            })
            .collect();
        // A place has a mark of how its value is given only if the type
        // that it is given as has one.
        // The receiver of a method of a type parameter has one if the
        // method that is called gets a copy of it: no method of an
        // interface does, none that takes the address of its receiver,
        // and neither the copy method nor an operator or a conversion,
        // which read the receiver where it is (PAR-5).
        let receiver_copied = |selector: &ExprId| {
            let (Some(selection), Some(&name)) =
                (selections.get(selector), self.param_methods.get(selector))
            else {
                return false;
            };
            let holder = selection.holder;
            let text = names.resolve(&name);
            let unexported = !text.starts_with(char::is_uppercase);
            let marks = types.method_marks(holder, name, unexported.then_some(package));
            !types.is_interface(holder)
                && !pointer_receivers.contains(selector)
                && text != "Clone"
                && !marks.has(Marks::OPERATOR)
                && !marks.has(Marks::CONVERT)
                && types.given_is_marked(holder)
        };
        let var_modes: FxHashMap<_, _> = (self.var_modes.iter())
            .filter(|&(expr, _)| {
                match (self.given_types.get(expr), self.receiver_marks.get(expr)) {
                    (Some(&given), _) => types.given_is_marked(ty(given)),
                    (None, Some(selector)) => receiver_copied(selector),
                    (None, None) => true,
                }
            })
            .map(|(&expr, &how)| (expr, how))
            .collect();
        let marked = |all: &rustc_hash::FxHashSet<_>| {
            (all.iter().copied())
                .filter(|expr| var_modes.contains_key(expr))
                .collect()
        };
        let (self_moves, last_uses) = (marked(&self.self_moves), marked(&self.last_uses));
        Body {
            locals,
            uses: self.uses.clone(),
            keys: self.keys.clone(),
            defs: self.defs.clone(),
            switch_vars: self.switch_vars.clone(),
            types: expr_types,
            selections,
            writes_through: self.writes_through.clone(),
            iface_conversions,
            instances,
            type_params: Vec::new(),
            given_types: FxHashMap::default(),
            param_methods: FxHashMap::default(),
            receiver_marks: FxHashMap::default(),
            bangs: self.bangs.clone(),
            var_modes,
            iface_args: self.iface_args.clone(),
            narrowed: self.narrowed.clone(),
            captures: self.captures.clone(),
            method_values: self.method_values.clone(),
            swaps: self.swaps.clone(),
            self_moves,
            last_uses,
            operators: op_uses,
            op_assigns,
            index_stores,
            case_operators,
            range_operators,
            iface_changes,
            part_conversions,
            case_types,
            foreign_uses,
            hidden_results: self.hidden_results.clone(),
        }
    }
}
