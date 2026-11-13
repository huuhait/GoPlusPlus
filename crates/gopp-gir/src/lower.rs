//! From a checked package to its module (gir.md §1, §3, §15).
//!
//! The bodies are lowered side by side, in runs that each fill a module
//! of their own; `merge` then makes one module of them, in which every
//! type, string and function has the place that the order of the source
//! gives it. So the module is the same for any number of threads.

mod assign;
mod equal;
mod expr;
mod facts;
mod func;
mod glue;
mod iface;
mod map;
mod merge;
mod slice;
mod stmt;
mod types;

#[cfg(test)]
mod tests;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_mangle::{Entity, Locals, Namer, symbol_of};
use gopp_sema::types::{NamedId, Type as CheckedType, TypeId as CheckedTypeId, Types};
use gopp_sema::{Body, LocalKind, ObjId, ObjKind, Package, Ref};
use gopp_span::SourceFile;
use gopp_syntax::ast::{DeclId, DeclKind, ExprId, ExprKind, File, Param, UnaryOp};
use gopp_target::TargetLayout;
use lasso::{Spur, ThreadedRodeo};
use rayon::prelude::*;
use rustc_hash::FxHashMap;

use crate::verify::verify_module;
use crate::{Global, GlobalId, Linkage, Module, TypeId};

/// A package as the checker left it.
pub struct Checked<'a> {
    pub package: &'a Package,
    /// The files in the order in which they were checked.
    pub files: &'a [(&'a File, &'a SourceFile)],
    pub types: &'a Types,
    pub names: &'a ThreadedRodeo,
}

/// The least source that one run takes: lowering it costs more than
/// starting a run and merging what it made.
const SOURCE_EACH: usize = 256 * 1024;

/// The module of a package that the checker accepted. What the lowering
/// does not support yet is reported to `sink` as `E0439`, each place
/// once, and has a zero of its type in the module; the module is
/// verified, and a rule that it breaks is reported as `E0440`.
pub fn lower_package(checked: &Checked<'_>, layout: TargetLayout, sink: &Sink) -> Module {
    let (module, diagnostics, _) = lower_counted(checked, layout, SOURCE_EACH);
    for diagnostic in diagnostics {
        sink.push(diagnostic);
    }
    module
}

/// `lower_package`, with what it reports and the steps that it took.
/// A run takes `source_each` bytes of source or more.
pub(crate) fn lower_counted(
    checked: &Checked<'_>,
    layout: TargetLayout,
    source_each: usize,
) -> (Module, Vec<Diagnostic>, u64) {
    let shared = Shared::new(checked, layout);
    let source: usize = shared.jobs.iter().map(|job| job.source).sum();
    let known: u64 = shared.jobs.iter().map(|job| job.facts.steps).sum();
    let runs = rayon::current_num_threads().min(source / source_each.max(1));
    // Runs of about as much source each, in the order of the source.
    let mut cut: Vec<&[Job]> = Vec::new();
    let mut rest = &shared.jobs[..];
    let mut left = source;
    for runs_left in (1..=runs.max(1)).rev() {
        let wanted = left / runs_left;
        let mut taken = 0;
        let mut count = 0;
        while count < rest.len() && (taken < wanted || runs_left == 1) {
            taken += rest[count].source;
            count += 1;
        }
        let (run, after) = rest.split_at(count);
        cut.push(run);
        rest = after;
        left -= taken;
    }
    let mut parts = vec![shared.globals()];
    parts.par_extend(cut.par_iter().map(|run| shared.run(run)));
    // The glue that the functions ask for is made last, each once.
    let asked: Vec<_> = parts
        .iter_mut()
        .flat_map(|part| part.glues.drain(..))
        .collect();
    if !asked.is_empty() {
        parts.push(shared.glue(asked));
    }
    let mut diagnostics = Vec::new();
    let mut steps = known;
    for part in &mut parts {
        diagnostics.append(&mut part.diagnostics);
        steps += part.steps;
    }
    let sources = parts.into_iter().map(|part| part.source).collect();
    let (module, merged) = merge::merge(&shared, sources);
    steps += merged;
    // What is not supported has a zero in its place, so the module is
    // whole: a rule that it breaks is a fault of the lowering. It is
    // reported at the start of the package.
    let start = shared.files.first().and_then(|(tree, _)| {
        let first = tree.decl_nodes.iter().next().map(|(_, decl)| decl.span);
        tree.package.map(|name| name.span).or(first)
    });
    if let (Err(errors), Some(span)) = (verify_module(&module), start) {
        for error in errors.iter().take(MAX_VERIFY_ERRORS) {
            let message = format!("the module of the package breaks a rule: {error}");
            diagnostics.push(Diagnostic::new(Code::GirVerify, span, message));
        }
    }
    (module, diagnostics, steps)
}

/// How many broken rules of one module are reported.
const MAX_VERIFY_ERRORS: usize = 8;

/// A function with a body.
pub(crate) struct Job {
    file: u32,
    /// Which of the declarations of the file, and of its bodies.
    at: usize,
    decl: DeclId,
    named: JobName,
    /// Empty while it is not known, and for what has none.
    symbol: String,
    /// The type of the function as the checker has it, the receiver
    /// first.
    receiver: Option<CheckedTypeId>,
    sig: CheckedTypeId,
    /// How many bytes of source it is.
    source: usize,
    /// Which parameters it only reads, and which variables are reached.
    facts: facts::Facts,
}

/// What a function with a body is, for its symbol.
#[derive(Clone, Copy)]
enum JobName {
    Object(ObjId),
    /// A method of the declared type.
    Method(CheckedTypeId, Spur),
    /// An `init` function, and its number once the files are counted.
    Init(u64),
}

/// A method of a type of the package.
pub(crate) struct Method {
    pub symbol: String,
    /// The type of the receiver: the type, or a pointer to it.
    pub receiver: CheckedTypeId,
    /// The type of the method without its receiver.
    pub sig: CheckedTypeId,
    /// It is declared in a Go++ file, where a value has the methods
    /// that take its address too (TYP-5).
    pub gopp: bool,
}

/// What every run reads and none writes.
pub(crate) struct Shared<'a> {
    pub package: &'a Package,
    pub files: &'a [(&'a File, &'a SourceFile)],
    pub types: &'a Types,
    pub names: &'a ThreadedRodeo,
    pub layout: TargetLayout,
    /// The path of the package, as a name.
    pub path: Spur,
    /// The name `_`.
    pub blank: Spur,
    /// The names of the method that ends a value and of the method
    /// that copies one.
    pub drop: Spur,
    pub clone: Spur,
    jobs: Vec<Job>,
    pub methods: FxHashMap<(NamedId, Spur), Method>,
    /// For each type that a function declares: which job, and which of
    /// the types of its name there, from 0.
    locals: FxHashMap<NamedId, (usize, u64)>,
    /// What the checker found in no function: for the glue, which is
    /// lowered from no source.
    no_body: Body,
    no_facts: facts::Facts,
    /// For the symbol of each function of the package: which of its
    /// parameters it only reads, the receiver first. A caller lends its
    /// arguments to those.
    pub lent: FxHashMap<String, Vec<bool>>,
}

/// A function that is called or a variable that is used, as the source
/// names it.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum CalleeKey {
    /// A function or a variable of the package.
    Object(ObjId),
    /// One of an imported package: the import and the object.
    Member(ObjId, ObjId),
    /// A method of the type.
    Method(CheckedTypeId, Spur),
}

/// A function that a run calls: where it is in the module of the run,
/// and what it takes.
#[derive(Clone)]
pub(crate) struct Callee {
    pub func: crate::FuncId,
    pub sig: TypeId,
    pub params: std::rc::Rc<[TypeId]>,
    /// How many of the last parameters are places for views that the
    /// results may be.
    pub views: usize,
    /// Which of the parameters the function only reads: it is lent
    /// those. Empty for a function whose body is not known here.
    pub lent: std::rc::Rc<[bool]>,
    pub ret: TypeId,
}

/// What one run made.
pub(crate) struct Part {
    /// Its module, with what each function of it names.
    source: merge::Source,
    diagnostics: Vec<Diagnostic>,
    steps: u64,
    /// The glue that its functions ask for.
    glues: Vec<Asked>,
}

/// A function that the package makes once for all that ask for it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum Made {
    /// The glue of a kind for a type.
    Glue(glue::Glue),
    /// What a table of methods holds for the method `name` of a
    /// declared type: in the table of the type itself, or in that of
    /// the pointers to it if `pointer` is set.
    Thunk { name: Spur, pointer: bool },
}

/// A function that is asked for: its symbol, what it is and its type.
pub(crate) type Asked = (String, Made, CheckedTypeId);

/// What a run keeps beside its module: what it has lowered and declared
/// already.
pub(crate) struct Memo<'a> {
    /// What makes the symbols, and the names of the declared structs.
    pub namer: Namer<'a>,
    pub types: FxHashMap<CheckedTypeId, Result<TypeId, &'static str>>,
    /// The type that every map has, once a map type is lowered.
    pub map: Option<TypeId>,
    /// The type that every interface value has, once an interface type
    /// is lowered.
    pub iface: Option<TypeId>,
    /// The descriptor of each type whose values the run has put into
    /// an interface value.
    pub infos: FxHashMap<CheckedTypeId, Result<GlobalId, &'static str>>,
    /// The methods of each interface type that the run has asked a
    /// value for while the program runs.
    pub itypes: FxHashMap<CheckedTypeId, Result<GlobalId, &'static str>>,
    /// The table of the methods of an interface type for the values of
    /// a type, for each pair that the run has used: the type, then the
    /// interface type.
    pub itabs: FxHashMap<(CheckedTypeId, CheckedTypeId), Result<GlobalId, &'static str>>,
    /// What is known of each map type that the run has used.
    pub maps: FxHashMap<CheckedTypeId, Result<map::MapOf, &'static str>>,
    /// Whether two values of a type are equal exactly when their bytes
    /// are.
    pub by_bytes: FxHashMap<CheckedTypeId, bool>,
    /// Whether a value of a type of the module owns nothing.
    pub flat: FxHashMap<TypeId, bool>,
    /// The glue of each kind and type that the run has asked for, as
    /// it is declared in the module of the run.
    pub glue_of: FxHashMap<(glue::Glue, CheckedTypeId), Result<crate::FuncId, &'static str>>,
    /// The glue that the package is to make: the symbol, the kind and
    /// the type.
    pub glues: Vec<Asked>,
    /// The types that are being lowered: one that is met again holds
    /// itself.
    pub open: Vec<CheckedTypeId>,
    /// The structs of the module whose types have a `drop` method: a
    /// value of one ends by a call of it, whatever its fields are.
    pub dropped: rustc_hash::FxHashSet<TypeId>,
    pub funcs: FxHashMap<String, crate::FuncId>,
    pub globals: FxHashMap<String, GlobalId>,
    /// The global of each variable that the run has named.
    pub global_keys: FxHashMap<CalleeKey, GlobalId>,
    /// The type of GIR for each type that an expression or a variable
    /// has: the empty type for a constant without a type.
    pub expr_types: FxHashMap<CheckedTypeId, Result<TypeId, &'static str>>,
    /// What is known of each function that the run calls.
    pub callees: FxHashMap<CalleeKey, Result<Callee, &'static str>>,
    /// The text of the names that the run has asked for.
    pub names: FxHashMap<Spur, Box<str>>,
    /// Which field of a struct has a name.
    pub fields: FxHashMap<(TypeId, Spur), Option<usize>>,
    pub diagnostics: Vec<Diagnostic>,
    pub steps: u64,
}

impl<'a> Memo<'a> {
    fn new(shared: &'a Shared<'_>, locals: &'a Locals<'a>) -> Self {
        Self {
            namer: Namer::new(shared.types, shared.names).with_locals(locals),
            types: FxHashMap::default(),
            map: None,
            iface: None,
            infos: FxHashMap::default(),
            itypes: FxHashMap::default(),
            itabs: FxHashMap::default(),
            maps: FxHashMap::default(),
            by_bytes: FxHashMap::default(),
            flat: FxHashMap::default(),
            glue_of: FxHashMap::default(),
            glues: Vec::new(),
            open: Vec::new(),
            dropped: rustc_hash::FxHashSet::default(),
            funcs: FxHashMap::default(),
            globals: FxHashMap::default(),
            global_keys: FxHashMap::default(),
            expr_types: FxHashMap::default(),
            callees: FxHashMap::default(),
            names: FxHashMap::default(),
            fields: FxHashMap::default(),
            diagnostics: Vec::new(),
            steps: 0,
        }
    }

    /// The text of the name, asked of the table of names once.
    pub fn name(&mut self, shared: &Shared<'_>, name: Spur) -> &str {
        self.names
            .entry(name)
            .or_insert_with(|| shared.names.resolve(&name).into())
    }
}

impl<'a> Shared<'a> {
    fn new(checked: &'a Checked<'a>, layout: TargetLayout) -> Self {
        let mut shared = Self {
            package: checked.package,
            files: checked.files,
            types: checked.types,
            names: checked.names,
            layout,
            path: checked.names.get_or_intern(&checked.package.path),
            blank: checked.names.get_or_intern_static("_"),
            drop: checked.names.get_or_intern_static("drop"),
            clone: checked.names.get_or_intern_static("Clone"),
            jobs: Vec::new(),
            methods: FxHashMap::default(),
            locals: FxHashMap::default(),
            no_body: Body::default(),
            no_facts: facts::Facts::default(),
            lent: FxHashMap::default(),
        };
        // The `init` functions count in the order of the names of their
        // files, then of the source.
        let mut inits: Vec<usize> = Vec::new();
        let files = shared.files;
        for (file, (tree, _)) in files.iter().enumerate() {
            for (at, &decl) in tree.decls.iter().enumerate() {
                if let Some(job) = shared.job(file as u32, at, decl) {
                    if matches!(job.named, JobName::Init(_)) {
                        inits.push(shared.jobs.len());
                    }
                    shared.jobs.push(job);
                }
            }
        }
        let place = |job: &Job| (files[job.file as usize].1.path(), job.at);
        inits.sort_by_key(|&at| place(&shared.jobs[at]));
        for (number, at) in inits.into_iter().enumerate() {
            shared.jobs[at].named = JobName::Init(number as u64);
        }
        let mut namer = Namer::new(shared.types, shared.names);
        let symbols: Vec<String> = (shared.jobs.iter())
            .map(|job| symbol_of(&mut namer, shared.entity(job.named)).unwrap_or_default())
            .collect();
        for (at, symbol) in symbols.into_iter().enumerate() {
            let job = &mut shared.jobs[at];
            job.symbol = symbol;
            if let (JobName::Method(named, name), Some(receiver)) = (job.named, job.receiver)
                && let CheckedType::Named { id, .. } = &*shared.types.get(named)
            {
                let dialect = shared.files[job.file as usize].1.dialect();
                let method = Method {
                    symbol: job.symbol.clone(),
                    receiver,
                    sig: job.sig,
                    gopp: dialect == gopp_span::Dialect::Gopp,
                };
                shared.methods.insert((*id, name), method);
            }
            let body = &shared.package.files[job.file as usize].bodies[job.at];
            let mut count: FxHashMap<Spur, u64> = FxHashMap::default();
            for local in &body.locals {
                if let (LocalKind::Type, CheckedType::Named { id, .. }) =
                    (local.kind, &*shared.types.get(local.ty))
                {
                    let ordinal = count.entry(local.name).or_default();
                    shared.locals.insert(*id, (at, *ordinal));
                    *ordinal += 1;
                }
            }
        }
        // What each function does to its parameters and its variables,
        // which every caller of it reads.
        let facts: Vec<_> = (shared.jobs.par_iter())
            .map(|job| facts::of(&shared, job))
            .collect();
        for (job, facts) in shared.jobs.iter_mut().zip(facts) {
            if !job.symbol.is_empty() {
                shared.lent.insert(job.symbol.clone(), facts.lent.clone());
            }
            job.facts = facts;
        }
        shared
    }

    /// What the function of a job is, for its symbol.
    fn entity(&self, named: JobName) -> Entity<'a> {
        match named {
            JobName::Object(object) => Entity::Object(self.package, object),
            JobName::Method(receiver, name) => Entity::Method { receiver, name },
            JobName::Init(number) => Entity::InitFunc(self.package, number),
        }
    }

    /// The function that declares the type, and which of the types of
    /// its name there the type is.
    fn local(&self, named: NamedId) -> Option<(Entity<'a>, u64)> {
        let &(job, ordinal) = self.locals.get(&named)?;
        Some((self.entity(self.jobs[job].named), ordinal))
    }

    /// The function that the declaration makes, if it has a body that
    /// is lowered as it is: no type parameters, a name. Its symbol is
    /// made when the `init` functions are counted.
    fn job(&self, file: u32, at: usize, decl: DeclId) -> Option<Job> {
        let (package, types, names) = (self.package, self.types, self.names);
        let (tree, _) = self.files[file as usize];
        let node = &tree.decl_nodes[decl];
        let DeclKind::Func {
            is_macro: false,
            receiver,
            name,
            type_params,
            body: Some(_),
            ..
        } = &node.kind
        else {
            return None;
        };
        let text = names.resolve(&name.name);
        if text == "_" || !type_params.is_empty() {
            return None;
        }
        let source = (node.span.hi - node.span.lo) as usize;
        let job = |named: JobName, receiver, sig| Job {
            file,
            at,
            decl,
            named,
            symbol: String::new(),
            receiver,
            sig,
            source,
            facts: facts::Facts::default(),
        };
        let Some(receiver) = receiver else {
            if text == "init" {
                let sig = types.func(&[], &[], false);
                return Some(job(JobName::Init(0), None, sig));
            }
            let id = *package.scope.get(&name.name)?;
            let object = package.object(id);
            return match object.kind {
                ObjKind::Func if object.span == name.span => {
                    Some(job(JobName::Object(id), None, object.ty))
                }
                _ => None,
            };
        };
        let body = &package.files[file as usize].bodies[at];
        let (named, pointer) = self.receiver(tree, body, receiver)?;
        let kind = types.get(named);
        let CheckedType::Named { id, args } = &*kind else {
            return None;
        };
        if !args.is_empty() || !types.type_params(*id).is_empty() {
            return None;
        }
        let found = types.lookup(named, name.name, self.path, names, false);
        let gopp_sema::types::Member::Method { sig, .. } = found.ok()?.member else {
            return None;
        };
        let receiver = match pointer {
            true => types.pointer(named),
            false => named,
        };
        Some(job(JobName::Method(named, name.name), Some(receiver), sig))
    }

    /// The declared type that a receiver names, and whether the
    /// receiver is a pointer to it.
    fn receiver(
        &self,
        tree: &File,
        body: &Body,
        receiver: &Param,
    ) -> Option<(CheckedTypeId, bool)> {
        let mut base = receiver.ty;
        let mut pointer = false;
        loop {
            match tree.exprs[base].kind {
                ExprKind::Paren(x) => base = x,
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => (base, pointer) = (x, true),
                _ => break,
            }
        }
        match body.uses.get(&base)? {
            Ref::Object(object) => Some((self.package.object(*object).ty, pointer)),
            _ => None,
        }
    }

    /// The variables of the package, in a module that is merged first.
    fn globals(&self) -> Part {
        let mut module = self.module();
        let locals = |named: NamedId| self.local(named);
        let mut memo = Memo::new(self, &locals);
        for (at, object) in self.package.objects.iter().enumerate() {
            if !matches!(object.kind, ObjKind::Var) {
                continue;
            }
            // A variable named `_` has no symbol, and no place.
            let entity = Entity::Object(self.package, ObjId(at as u32));
            let Ok(name) = symbol_of(&mut memo.namer, entity) else {
                continue;
            };
            let lowered = memo.lower_type(self, &mut module.types, object.ty);
            if let Some(ty) = lowered.ok().filter(|&ty| ty != crate::TypeInterner::UNIT) {
                module.globals.alloc(Global {
                    name,
                    ty,
                    link: Linkage::External,
                    mutable: true,
                    parts: Vec::new(),
                });
            }
        }
        // No function runs the first values of the variables yet: a
        // variable would silently start as zero.
        for first in &self.package.init_order {
            if let Some((tree, _)) = self.files.get(first.file as usize) {
                let span = tree.exprs[first.value].span;
                let message = "not supported yet: a first value of a variable of the package";
                memo.diagnostics.push(Diagnostic::new(
                    Code::GirUnsupported,
                    span,
                    message.to_owned(),
                ));
            }
        }
        Part {
            source: merge::Source::new(module),
            diagnostics: memo.diagnostics,
            steps: memo.steps,
            glues: Vec::new(),
        }
    }

    /// An empty module of the package.
    pub fn module(&self) -> Module {
        Module {
            name: self.package.path.clone(),
            files: self
                .files
                .iter()
                .map(|(_, source)| source.path().display().to_string())
                .collect(),
            ..Module::default()
        }
    }

    /// The functions of the run, in a module of their own.
    fn run(&self, jobs: &[Job]) -> Part {
        let mut module = self.module();
        let locals = |named: NamedId| self.local(named);
        let mut memo = Memo::new(self, &locals);
        for job in jobs {
            func::lower_function(self, &mut module, &mut memo, job);
        }
        Part {
            source: merge::Source::new(module),
            diagnostics: memo.diagnostics,
            steps: memo.steps,
            glues: memo.glues,
        }
    }

    /// The glue that the functions of the package asked for, and what
    /// that glue asks for itself, in a module of its own: each once, in
    /// the order of the symbols.
    fn glue(&self, mut asked: Vec<Asked>) -> Part {
        let mut module = self.module();
        let locals = |named: NamedId| self.local(named);
        let mut memo = Memo::new(self, &locals);
        asked.sort();
        let mut made = rustc_hash::FxHashSet::default();
        let mut at = 0;
        while at < asked.len() {
            let (symbol, kind, ty) = asked[at].clone();
            at += 1;
            if made.insert(symbol.clone()) {
                match kind {
                    Made::Glue(kind) => {
                        func::lower_glue(self, &mut module, &mut memo, &symbol, kind, ty);
                    }
                    Made::Thunk { name, pointer } => {
                        func::lower_thunk(
                            self,
                            &mut module,
                            &mut memo,
                            &symbol,
                            (ty, name, pointer),
                        );
                    }
                }
                asked.append(&mut memo.glues);
            }
        }
        Part {
            source: merge::Source::new(module),
            diagnostics: memo.diagnostics,
            steps: memo.steps,
            glues: Vec::new(),
        }
    }

    /// The expression without the parentheses around it.
    pub fn unparen(tree: &File, mut expr: ExprId) -> ExprId {
        while let ExprKind::Paren(inner) = tree.exprs[expr].kind {
            expr = inner;
        }
        expr
    }
}
