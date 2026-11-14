//! One module from the modules of the runs. The functions of the
//! package come first, in the order of the source; every type, string,
//! global and function of another package gets its number where a walk
//! over those functions first meets it. So the module does not depend
//! on how the functions were cut into runs.
//!
//! Each run lists what each of its functions names, side by side with
//! the other runs; the numbers are given in one pass over those lists,
//! which is short; then each run rewrites its functions, side by side
//! again.

use la_arena::Arena;
use rayon::prelude::*;
use rustc_hash::{FxHashMap, FxHashSet};

use super::Shared;
use crate::{
    AggKind, Constant, ConstantKind, FuncId, FuncType, Function, Global, GlobalId, Module, Operand,
    Place, Rvalue, ScopeKind, StatementKind, TerminatorKind, Type, TypeId, TypeInterner,
};

/// What a walk over a function does with each thing that it names.
trait Names {
    fn ty(&mut self, ty: &mut TypeId);
    fn func(&mut self, func: &mut FuncId);
    fn global(&mut self, global: &mut GlobalId);
    fn string(&mut self, string: &mut u32);
}

/// What one function is the first of its run to name, in the order in
/// which a walk meets it; and its strings, as often as it names them.
/// What an earlier function of the run names has its number before
/// this function is looked at.
#[derive(Default)]
struct Named {
    types: Vec<TypeId>,
    funcs: Vec<FuncId>,
    globals: Vec<GlobalId>,
    strings: Vec<u32>,
    steps: u64,
}

/// What the functions of a run have named so far.
#[derive(Default)]
struct Seen {
    types: FxHashSet<TypeId>,
    funcs: FxHashSet<FuncId>,
    globals: FxHashSet<GlobalId>,
}

struct Naming<'s> {
    named: Named,
    seen: &'s mut Seen,
}

impl Names for Naming<'_> {
    fn ty(&mut self, ty: &mut TypeId) {
        self.named.steps += 1;
        if self.seen.types.insert(*ty) {
            self.named.types.push(*ty);
        }
    }

    fn func(&mut self, func: &mut FuncId) {
        if self.seen.funcs.insert(*func) {
            self.named.funcs.push(*func);
        }
    }

    fn global(&mut self, global: &mut GlobalId) {
        if self.seen.globals.insert(*global) {
            self.named.globals.push(*global);
        }
    }

    fn string(&mut self, string: &mut u32) {
        self.named.strings.push(*string);
    }
}

/// Where the things that a run made are in the merged module.
struct Places<'s> {
    type_of: &'s FxHashMap<TypeId, TypeId>,
    func_of: &'s [Option<FuncId>],
    global_of: &'s [Option<GlobalId>],
    /// Where the strings of the function start in the merged module;
    /// they follow one another as the walk meets them.
    string: u32,
}

impl Names for Places<'_> {
    fn ty(&mut self, ty: &mut TypeId) {
        if let Some(&to) = self.type_of.get(ty) {
            *ty = to;
        }
    }

    fn func(&mut self, func: &mut FuncId) {
        if let Some(Some(to)) = self.func_of.get(func.into_raw().into_u32() as usize) {
            *func = *to;
        }
    }

    fn global(&mut self, global: &mut GlobalId) {
        if let Some(Some(to)) = self.global_of.get(global.into_raw().into_u32() as usize) {
            *global = *to;
        }
    }

    fn string(&mut self, string: &mut u32) {
        *string = self.string;
        self.string += 1;
    }
}

/// Walks the function in the order in which it names its parts.
fn walk(func: &mut Function, names: &mut impl Names) {
    names.ty(&mut func.sig);
    for local in func.locals.values_mut() {
        names.ty(&mut local.ty);
    }
    for scope in &mut func.scopes {
        if let ScopeKind::Inlined(callee) = &mut scope.kind {
            names.func(callee);
        }
    }
    for block in func.blocks.values_mut() {
        for stmt in &mut block.stmts {
            if let StatementKind::Assign(assigned) = &mut stmt.kind {
                let (dest, value) = &mut **assigned;
                place(dest, names);
                rvalue(value, names);
            }
        }
        match &mut block.term.kind {
            TerminatorKind::Switch { on, .. } => operand(on, names),
            TerminatorKind::Call {
                callee, args, dest, ..
            } => {
                operand(callee, names);
                args.iter_mut().for_each(|arg| operand(arg, names));
                place(dest, names);
            }
            TerminatorKind::Drop { place: dropped, .. } => place(dropped, names),
            TerminatorKind::Assert { cond, args, .. } => {
                operand(cond, names);
                args.iter_mut().for_each(|arg| operand(arg, names));
            }
            TerminatorKind::Goto(_)
            | TerminatorKind::Return
            | TerminatorKind::Unreachable
            | TerminatorKind::Resume => {}
        }
    }
}

fn place(place: &mut Place, names: &mut impl Names) {
    for step in &mut place.projections {
        names.ty(&mut step.ty);
    }
}

fn operand(operand: &mut Operand, names: &mut impl Names) {
    match operand {
        Operand::Copy(read) | Operand::Move(read) => place(read, names),
        Operand::Const(constant) => self::constant(constant, names),
    }
}

fn constant(constant: &mut Constant, names: &mut impl Names) {
    names.ty(&mut constant.ty);
    match &mut constant.kind {
        ConstantKind::Str(string) => names.string(string),
        ConstantKind::Func(func) => names.func(func),
        ConstantKind::Global(global) | ConstantKind::Value(global) => names.global(global),
        ConstantKind::Int(_)
        | ConstantKind::Float(_)
        | ConstantKind::Bool(_)
        | ConstantKind::Null
        | ConstantKind::Zero => {}
    }
}

fn rvalue(value: &mut Rvalue, names: &mut impl Names) {
    match value {
        Rvalue::Use(read) | Rvalue::Unary(_, read) => operand(read, names),
        Rvalue::AddrOf(of, _) | Rvalue::Len(of) => place(of, names),
        Rvalue::Binary(_, first, second) => {
            operand(first, names);
            operand(second, names);
        }
        Rvalue::Cast(_, read, to) => {
            operand(read, names);
            names.ty(to);
        }
        Rvalue::Aggregate(kind, parts) => {
            match kind {
                AggKind::Tuple => {}
                AggKind::Array(of) | AggKind::Of(of) => names.ty(of),
            }
            parts.iter_mut().for_each(|part| operand(part, names));
        }
    }
}

/// One run, and where what it made is in the merged module.
pub(super) struct Source {
    types: TypeInterner,
    strings: Vec<Box<[u8]>>,
    /// Each function as a declaration, and the functions that have a
    /// body, each with what it names and where its strings start.
    heads: Vec<Function>,
    bodies: Vec<Option<(Function, Named, u32)>>,
    globals: Vec<Global>,
    type_of: FxHashMap<TypeId, TypeId>,
    /// The types that were mapped, in their order: those that were
    /// made for a type that turns out to be known are unmapped.
    mapped: Vec<TypeId>,
    func_of: Vec<Option<FuncId>>,
    global_of: Vec<Option<GlobalId>>,
    /// The globals of the run whose static data is still to be said in
    /// the merged module, each with its place there.
    unfilled: Vec<(usize, GlobalId)>,
}

/// The module that is being made.
struct Merged {
    module: Module,
    /// The functions without a body, which come after all that have one.
    declared: Vec<Function>,
    defined: usize,
    funcs: FxHashMap<String, FuncId>,
    globals: FxHashMap<String, GlobalId>,
    /// The globals that no run has said static data for yet.
    unsaid: FxHashSet<GlobalId>,
    steps: u64,
}

/// The module of the runs, in their order, and the steps of the merge.
pub(super) fn merge(shared: &Shared<'_>, mut sources: Vec<Source>) -> (Module, u64) {
    let mut merged = Merged {
        module: shared.module(),
        declared: Vec::new(),
        defined: 0,
        funcs: FxHashMap::default(),
        globals: FxHashMap::default(),
        unsaid: FxHashSet::default(),
        steps: 0,
    };
    // The functions with a body have the first numbers.
    let mut bodies: Vec<(usize, usize)> = Vec::new();
    for (part, source) in sources.iter().enumerate() {
        for (at, head) in source.heads.iter().enumerate() {
            if source.bodies[at].is_some() {
                let id = FuncId::from_raw((bodies.len() as u32).into());
                merged.funcs.entry(head.name.clone()).or_insert(id);
                bodies.push((part, at));
            }
        }
    }
    merged.defined = bodies.len();
    // The variables of the package, which the first part has.
    if let Some(first) = sources.first_mut() {
        for at in 0..first.globals.len() {
            first.global(&mut merged, at);
        }
    }
    // What each function names gets its number, in the order of the
    // functions.
    for &(part, at) in &bodies {
        let source = &mut sources[part];
        let Some((_, named, _)) = &mut source.bodies[at] else {
            continue;
        };
        let (types, funcs, globals, strings) = (
            std::mem::take(&mut named.types),
            std::mem::take(&mut named.funcs),
            std::mem::take(&mut named.globals),
            std::mem::take(&mut named.strings),
        );
        merged.steps += named.steps;
        for ty in types {
            source.ty(&mut merged.module.types, &mut merged.steps, ty);
        }
        for func in funcs {
            source.func(&mut merged, func.into_raw().into_u32() as usize);
        }
        for global in globals {
            source.global(&mut merged, global.into_raw().into_u32() as usize);
        }
        // A string of a run is named once.
        let start = merged.module.strings.len() as u32;
        for string in strings {
            let text = source.strings.get_mut(string as usize).map(std::mem::take);
            merged.module.strings.push(text.unwrap_or_default());
        }
        if let Some((_, _, strings_at)) = &mut source.bodies[at] {
            *strings_at = start;
        }
    }
    // Each run rewrites its own functions.
    sources.par_iter_mut().for_each(|source| {
        let (type_of, func_of, global_of) = (&source.type_of, &source.func_of, &source.global_of);
        for (func, _, strings_at) in source.bodies.iter_mut().flatten() {
            let mut places = Places {
                type_of,
                func_of,
                global_of,
                string: *strings_at,
            };
            walk(func, &mut places);
        }
    });
    let mut defined = Arena::new();
    for (part, at) in bodies {
        if let Some((func, ..)) = sources[part].bodies[at].take() {
            defined.alloc(func);
        }
    }
    defined.extend(merged.declared);
    merged.module.funcs = defined;
    // What is left of the runs is freed side by side.
    sources.into_par_iter().for_each(drop);
    (merged.module, merged.steps)
}

impl Source {
    /// The module of a run, with what each of its functions names.
    pub fn new(module: Module) -> Self {
        let heads: Vec<Function> = module
            .funcs
            .values()
            .map(|func| Function {
                name: func.name.clone(),
                locals: Arena::new(),
                blocks: Arena::new(),
                scopes: Vec::new(),
                ..*func
            })
            .collect();
        let mut seen = Seen::default();
        let bodies: Vec<Option<(Function, Named, u32)>> = module
            .funcs
            .into_iter()
            .map(|(_, mut func)| match func.is_declaration() {
                true => None,
                false => {
                    let mut naming = Naming {
                        named: Named::default(),
                        seen: &mut seen,
                    };
                    walk(&mut func, &mut naming);
                    Some((func, naming.named, 0))
                }
            })
            .collect();
        let globals: Vec<Global> = module
            .globals
            .into_iter()
            .map(|(_, global)| global)
            .collect();
        Self {
            types: module.types,
            strings: module.strings,
            func_of: vec![None; heads.len()],
            global_of: vec![None; globals.len()],
            heads,
            bodies,
            globals,
            type_of: FxHashMap::default(),
            mapped: Vec::new(),
            unfilled: Vec::new(),
        }
    }

    fn map(&mut self, from: TypeId, to: TypeId) {
        self.type_of.insert(from, to);
        self.mapped.push(from);
    }

    /// The type of the merged module that is the type `ty` of the run.
    /// A struct is the struct of its name; any other type is the type
    /// of the same parts.
    fn ty(&mut self, out: &mut TypeInterner, steps: &mut u64, ty: TypeId) -> TypeId {
        enum Task {
            Type(TypeId),
            /// The fields of a struct that the merged module did not
            /// have: the struct of the run, and that of the module.
            Fields(TypeId, TypeId),
        }
        if let Some(&known) = self.type_of.get(&ty) {
            return known;
        }
        let mut rest = vec![Task::Type(ty)];
        while let Some(task) = rest.pop() {
            *steps += 1;
            let (from, fields_of) = match task {
                Task::Type(from) => (from, None),
                Task::Fields(from, to) => (from, Some(to)),
            };
            if fields_of.is_none() && self.type_of.contains_key(&from) {
                continue;
            }
            let kind = self.types.get(from).clone();
            // The parts that are not known yet come first, then this
            // type again.
            let mut parts: Vec<TypeId> = Vec::new();
            match &kind {
                Type::Unit | Type::Bool | Type::Int { .. } | Type::Float { .. } | Type::String => {}
                Type::Ptr(of) | Type::Slice(of) | Type::Option(of) | Type::Array(of, _) => {
                    parts.push(*of);
                }
                Type::Tuple(of) => parts.extend(of.iter()),
                Type::Func(func) => {
                    parts.extend(func.params.iter());
                    parts.push(func.ret);
                }
                Type::Struct(id) => match fields_of {
                    Some(_) => parts.extend(self.types.struct_fields(*id).unwrap_or_default()),
                    None => {
                        let (to, new) = out.struct_named(self.types.struct_name(*id));
                        self.map(from, to);
                        if new {
                            rest.push(Task::Fields(from, to));
                        }
                        continue;
                    }
                },
                Type::Rec(id) => {
                    let name = self.types.rec_name(*id).to_owned();
                    let body = self.types.rec_body(*id);
                    let mark = self.mapped.len();
                    let mut made = None;
                    let to = out.rec(&name, |out, shell| {
                        made = Some(shell);
                        self.map(from, shell);
                        match body {
                            Some(body) => self.ty(out, steps, body),
                            None => shell,
                        }
                    });
                    // What was made with the shell is of no use if the
                    // module had a type of this shape.
                    let with_shell = self.mapped.split_off(mark);
                    if made != Some(to) {
                        for stale in with_shell {
                            self.type_of.remove(&stale);
                        }
                    }
                    self.map(from, to);
                    continue;
                }
            }
            let unknown: Vec<TypeId> = parts
                .iter()
                .copied()
                .filter(|part| !self.type_of.contains_key(part))
                .collect();
            if !unknown.is_empty() {
                rest.push(match fields_of {
                    Some(to) => Task::Fields(from, to),
                    None => Task::Type(from),
                });
                rest.extend(unknown.into_iter().map(Task::Type));
                continue;
            }
            let known = |part: &TypeId| self.type_of[part];
            if let (Some(to), Type::Struct(id)) = (fields_of, &kind) {
                let names = self.types.field_names(*id);
                let fields: Vec<(&str, TypeId)> = names
                    .iter()
                    .zip(&parts)
                    .map(|(name, part)| (name.as_str(), known(part)))
                    .collect();
                if let Type::Struct(to) = out.get(to).clone() {
                    // A struct that the module did not have has no
                    // fields yet.
                    let _ = out.set_fields(to, &fields);
                }
                continue;
            }
            let to = match &kind {
                Type::Ptr(of) => out.ptr(known(of)),
                Type::Slice(of) => out.intern(Type::Slice(known(of))),
                Type::Option(of) => out.intern(Type::Option(known(of))),
                Type::Array(of, count) => out.intern(Type::Array(known(of), *count)),
                Type::Tuple(of) => {
                    let of: Vec<TypeId> = of.iter().map(known).collect();
                    out.tuple(&of)
                }
                Type::Func(func) => out.intern(Type::Func(FuncType {
                    params: func.params.iter().map(known).collect(),
                    ret: known(&func.ret),
                    has_env: func.has_env,
                    cc: func.cc,
                })),
                plain => out.intern(plain.clone()),
            };
            self.map(from, to);
        }
        self.type_of[&ty]
    }

    /// The global of the merged module with the name of the global
    /// `at` of the run. Its static data goes with it, if the module
    /// has none for it yet: every run says the same.
    fn global(&mut self, merged: &mut Merged, at: usize) -> GlobalId {
        let id = self.placed(merged, at);
        // The data of a global may name a global with data of its own:
        // a list of what is left, as deep as the data goes.
        while let Some((at, id)) = self.unfilled.pop() {
            let parts = std::mem::take(&mut self.globals[at].parts);
            let mut filled = Vec::with_capacity(parts.len());
            for mut part in parts {
                merged.steps += 1;
                part.ty = self.ty(&mut merged.module.types, &mut merged.steps, part.ty);
                match &mut part.kind {
                    ConstantKind::Str(string) => {
                        let text = self.strings.get_mut(*string as usize).map(std::mem::take);
                        *string = merged.module.strings.len() as u32;
                        merged.module.strings.push(text.unwrap_or_default());
                    }
                    ConstantKind::Func(func) => {
                        *func = self.func(merged, func.into_raw().into_u32() as usize);
                    }
                    ConstantKind::Global(global) | ConstantKind::Value(global) => {
                        *global = self.placed(merged, global.into_raw().into_u32() as usize);
                    }
                    _ => {}
                }
                filled.push(part);
            }
            merged.module.globals[id].parts = filled;
        }
        id
    }

    /// The place of the global `at` of the run in the merged module.
    fn placed(&mut self, merged: &mut Merged, at: usize) -> GlobalId {
        if let Some(known) = self.global_of[at] {
            return known;
        }
        let global = &self.globals[at];
        let said = !global.parts.is_empty();
        let (id, new) = match merged.globals.get(&global.name) {
            Some(&id) => (id, false),
            None => {
                let mut global = Global {
                    parts: Vec::new(),
                    ..global.clone()
                };
                global.ty = self.ty(&mut merged.module.types, &mut merged.steps, global.ty);
                let name = global.name.clone();
                let id = merged.module.globals.alloc(global);
                merged.globals.insert(name, id);
                (id, true)
            }
        };
        self.global_of[at] = Some(id);
        if said && (new || merged.unsaid.remove(&id)) {
            self.unfilled.push((at, id));
        } else if new && !said {
            merged.unsaid.insert(id);
        }
        id
    }

    /// The function of the merged module with the name of the function
    /// `at` of the run: one of the package, or a declaration.
    fn func(&mut self, merged: &mut Merged, at: usize) -> FuncId {
        if let Some(known) = self.func_of[at] {
            return known;
        }
        let id = match merged.funcs.get(&self.heads[at].name) {
            Some(&id) => id,
            None => {
                let mut declared = self.heads[at].clone();
                declared.sig = self.ty(&mut merged.module.types, &mut merged.steps, declared.sig);
                let id = FuncId::from_raw(((merged.defined + merged.declared.len()) as u32).into());
                merged.funcs.insert(declared.name.clone(), id);
                merged.declared.push(declared);
                id
            }
        };
        self.func_of[at] = Some(id);
        id
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A module with a struct that points to itself and holds a type
    /// that holds itself.
    fn run() -> (Module, TypeId) {
        let mut module = Module::default();
        let types = &mut module.types;
        let int = types.int(64, true);
        let pair = types.tuple(&[int, TypeInterner::BOOL]);
        let list = types.rec("p.List", |types, me| types.intern(Type::Slice(me)));
        let (node, _) = types.struct_named("p.Node");
        let Type::Struct(id) = *types.get(node) else {
            panic!();
        };
        let next = types.ptr(node);
        let fields = [("next", next), ("all", list), ("pair", pair)];
        types.set_fields(id, &fields).unwrap();
        (module, node)
    }

    #[test]
    fn a_type_of_a_run_is_the_type_of_the_same_parts_in_the_module() {
        let mut out = TypeInterner::new();
        // The module has other types already, and one of the shape of
        // `p.List`.
        out.float(32);
        let known = out.rec("p.List", |types, me| types.intern(Type::Slice(me)));
        let mut steps = 0;
        let mut mapped = Vec::new();
        for _ in 0..2 {
            let (module, node) = run();
            let mut source = Source::new(module);
            mapped.push(source.ty(&mut out, &mut steps, node));
        }
        assert_eq!(mapped[0], mapped[1]);
        let Type::Struct(id) = *out.get(mapped[0]) else {
            panic!();
        };
        let fields = out.struct_fields(id).unwrap().to_vec();
        let shown: Vec<String> = fields.iter().map(|&field| out.display(field)).collect();
        assert_eq!(shown, ["*p.Node", "p.List", "(i64, bool)"]);
        assert_eq!(fields[1], known);
        assert_eq!(*out.get(fields[0]), Type::Ptr(mapped[0]));
    }
}
