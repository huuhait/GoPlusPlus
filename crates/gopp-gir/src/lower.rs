//! From a checked package to its module (gir.md §1, §3, §15).
//!
//! The bodies are lowered side by side, in runs that each fill a module
//! of their own; `merge` then makes one module of them, in which every
//! type, string and function has the place that the order of the source
//! gives it. So the module is the same for any number of threads.

mod assign;
mod closure;
mod equal;
mod expr;
mod facts;
mod func;
mod glue;
mod iface;
mod map;
mod merge;
mod panic;
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
use rustc_hash::{FxHashMap, FxHashSet};

use crate::verify::verify_module;
use crate::{Global, GlobalId, Linkage, Module, TypeId};

/// A package as the checker left it.
pub struct Checked<'a> {
    pub package: &'a Package,
    /// The files in the order in which they were checked.
    pub files: &'a [(&'a File, &'a SourceFile)],
    pub types: &'a Types,
    pub names: &'a ThreadedRodeo,
    /// A panic ends the program at once (`-panic=abort`, PAN-7): no
    /// deferred call is made for it and nothing is unwound.
    pub aborts: bool,
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
    let known = known + shared.found_steps;
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
    /// For an instance: which of the bodies that were made for the
    /// instances of the package is its own.
    made: Option<usize>,
}

/// What a function with a body is, for its symbol.
#[derive(Clone, Copy)]
enum JobName {
    Object(ObjId),
    /// A method of the declared type.
    Method(CheckedTypeId, Spur),
    /// An `init` function, and its number once the files are counted.
    Init(u64),
    /// A generic function with type arguments: the function, and the
    /// instance of the package that has the arguments.
    Instance(ObjId, usize),
    /// A method with type parameters of its own, of the type, with type
    /// arguments: the instance of the package has them last.
    OwnMethod(CheckedTypeId, Spur, usize),
}

/// A generic function, or a method with type parameters of its own of
/// a type, as a use names it.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum Used {
    Func(ObjId),
    Method(CheckedTypeId, Spur),
}

/// The body of a generic function, or of a method of a generic type,
/// with type arguments.
struct Instantiated {
    /// Those of the receiver, then those of the function itself.
    args: Vec<CheckedTypeId>,
    /// How many of them are of the receiver.
    of_receiver: usize,
    /// What the checker recorded for the generic body, read with the
    /// type arguments.
    body: Body,
}

/// A generic function of the package, or a method of a generic type of
/// it: where it is declared.
#[derive(Clone, Copy)]
struct Generic {
    file: u32,
    at: usize,
    decl: DeclId,
    /// For a method: its name, and whether it takes the address of its
    /// receiver.
    method: Option<(Spur, bool)>,
}

/// What a generic declaration with a body is of: a function of the
/// package, a generic type for a method, or a type for a method with
/// type parameters of its own.
enum GenericOf {
    Func(ObjId),
    Type(NamedId),
    Method(NamedId, Spur),
}

/// An instance that a level of functions uses first.
struct Found {
    of: Generic,
    named: Named,
    args: Vec<CheckedTypeId>,
    of_receiver: usize,
    sig: CheckedTypeId,
    symbol: String,
}

/// What an instance is an instance of.
#[derive(Clone, Copy)]
enum Named {
    Func(ObjId),
    /// A method of the instance of a generic type.
    Method(CheckedTypeId, Spur),
    /// A method with type parameters of its own, of the type.
    OwnMethod(CheckedTypeId, Spur),
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
    /// A panic ends the program at once.
    pub aborts: bool,
    /// The path of the package, as a name.
    pub path: Spur,
    /// The name `_`.
    pub blank: Spur,
    /// The names of the method that ends a value and of the method
    /// that copies one.
    pub drop: Spur,
    pub clone: Spur,
    /// The names of the methods by which the value of a panic says its
    /// text.
    pub error: Spur,
    pub string: Spur,
    jobs: Vec<Job>,
    /// The instances of generic functions, and of the methods of
    /// generic types, that the package uses, each once, in the order
    /// in which they are found: those that the functions use in the
    /// order of the source, then those that the instances found so far
    /// use (GEN-5).
    instances: Vec<Instantiated>,
    /// The job of a generic function, or of a method with type
    /// parameters of its own, with its type arguments.
    instance_jobs: FxHashMap<(Used, Vec<CheckedTypeId>), usize>,
    /// The methods of the instances of generic types, each by the
    /// instance and its name.
    instance_methods: FxHashMap<(CheckedTypeId, Spur), Method>,
    /// The steps that finding the instances took: one for each use of
    /// a generic function, and for each expression of a body that was
    /// made.
    found_steps: u64,
    pub methods: FxHashMap<(NamedId, Spur), Method>,
    /// For each type that a function declares: which job, and which of
    /// the types of its name there, from 0.
    locals: FxHashMap<NamedId, (usize, u64)>,
    /// What the checker found in no function: for the glue, which is
    /// lowered from no source.
    no_body: Body,
    no_facts: facts::Facts,
    /// The function literals of the package, each by its file and its
    /// expression.
    literals: FxHashMap<(u32, ExprId), Literal>,
    /// For the symbol of each function of the package: which of its
    /// parameters it does not own, the receiver first. A caller lends
    /// its arguments to those.
    pub lent: FxHashMap<String, Vec<bool>>,
    /// For the symbol of each function of the package: which of its
    /// parameters it calls methods through. A caller lends those a
    /// value that nothing else reads.
    pub worked: FxHashMap<String, Vec<bool>>,
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
    /// A generic function with type arguments, by its job.
    Instance(usize),
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
    /// Which of the parameters the function does not own: it is lent
    /// those. Empty for a function whose body is not known here.
    pub lent: std::rc::Rc<[bool]>,
    /// Which of the lent parameters the function calls methods through.
    pub worked: std::rc::Rc<[bool]>,
    pub ret: TypeId,
    /// For a function with `...`: the type of its last parameter, a
    /// slice, as the checker has it.
    pub rest: Option<CheckedTypeId>,
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

/// A function literal: where it is written, and its number there for
/// its symbol.
pub(crate) struct Literal {
    /// The declaration of its file that it is written in: that of a
    /// function, or of a generic function, whose instances each have
    /// the literal.
    at: usize,
    /// The function literal that it is written directly in, if it is
    /// in one.
    within: Option<ExprId>,
    /// From 1 in the order of the source, among the literals that are
    /// written directly in the same function.
    ordinal: u64,
}

/// A function that the package makes once for all that ask for it.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum Made {
    /// The glue of a kind for a type.
    Glue(glue::Glue),
    /// What a table of methods holds for the method `name` of a
    /// declared type: in the table of the type itself, or in that of
    /// the pointers to it if `pointer` is set.
    Thunk { name: Spur, pointer: bool },
    /// The code of the function with the symbol as a value: the
    /// function with an environment in front, which it does not read.
    Value(String),
    /// The code of a method value: the method `name` of a declared
    /// type or of an interface type, with its receiver in the
    /// environment.
    MethodValue(Spur),
    /// The code of a method expression: the method `name` of a
    /// declared type as a function that takes its receiver first, by
    /// its address if `pointer` is set and the method takes a value.
    MethodExpr { name: Spur, pointer: bool },
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
    /// The types that function values have: two words, the code and
    /// its environment.
    pub func_types: rustc_hash::FxHashSet<TypeId>,
    /// The function literals that the functions of the run name, which
    /// are lowered after the function that is lowered now.
    pub literals: Vec<(&'a Job, ExprId)>,
    queued: rustc_hash::FxHashSet<(*const Job, ExprId)>,
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
            func_types: rustc_hash::FxHashSet::default(),
            literals: Vec::new(),
            queued: rustc_hash::FxHashSet::default(),
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

    /// The symbol of the function literal that is written in the
    /// function of the job; none for a literal that is not in it.
    pub fn literal(&mut self, shared: &Shared<'_>, job: &Job, expr: ExprId) -> Option<String> {
        let node = self.literal_node(shared, job, expr)?;
        Some(self.namer.symbol(node))
    }

    fn literal_node(
        &mut self,
        shared: &Shared<'_>,
        job: &Job,
        expr: ExprId,
    ) -> Option<gopp_mangle::NodeId> {
        let literal = shared.literals.get(&(job.file, expr))?;
        if literal.at != job.at {
            return None;
        }
        let within = match literal.within {
            Some(within) => func::deeper(|| self.literal_node(shared, job, within))?,
            None => self.namer.entity(shared.entity(job.named)).ok()?,
        };
        self.namer.closure(within, literal.ordinal).ok()
    }

    /// The symbol of the function that makes the deferred calls of a
    /// function of the package, or of a function literal in it: that
    /// of a literal with the number 0 there, which no literal of the
    /// source has.
    pub fn deferred(
        &mut self,
        shared: &Shared<'_>,
        job: &Job,
        literal: Option<ExprId>,
    ) -> Option<String> {
        let within = match literal {
            Some(literal) => self.literal_node(shared, job, literal)?,
            None => self.namer.entity(shared.entity(job.named)).ok()?,
        };
        let node = self.namer.closure(within, 0).ok()?;
        Some(self.namer.symbol(node))
    }

    /// The literal is to be lowered, once.
    pub fn queue(&mut self, job: &'a Job, expr: ExprId) {
        if self.queued.insert((std::ptr::from_ref(job), expr)) {
            self.literals.push((job, expr));
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
            aborts: checked.aborts,
            path: checked.names.get_or_intern(&checked.package.path),
            blank: checked.names.get_or_intern_static("_"),
            drop: checked.names.get_or_intern_static("drop"),
            clone: checked.names.get_or_intern_static("Clone"),
            error: checked.names.get_or_intern_static("Error"),
            string: checked.names.get_or_intern_static("String"),
            jobs: Vec::new(),
            instances: Vec::new(),
            instance_jobs: FxHashMap::default(),
            instance_methods: FxHashMap::default(),
            found_steps: 0,
            methods: FxHashMap::default(),
            locals: FxHashMap::default(),
            no_body: Body::default(),
            no_facts: facts::Facts::default(),
            literals: FxHashMap::default(),
            lent: FxHashMap::default(),
            worked: FxHashMap::default(),
        };
        // The `init` functions count in the order of the names of their
        // files, then of the source.
        let mut inits: Vec<usize> = Vec::new();
        let mut generics: FxHashMap<ObjId, Generic> = FxHashMap::default();
        let mut methods: FxHashMap<NamedId, Vec<Generic>> = FxHashMap::default();
        let mut own: FxHashMap<(NamedId, Spur), Generic> = FxHashMap::default();
        let files = shared.files;
        for (file, (tree, _)) in files.iter().enumerate() {
            for (at, &decl) in tree.decls.iter().enumerate() {
                if let Some(job) = shared.job(file as u32, at, decl) {
                    if matches!(job.named, JobName::Init(_)) {
                        inits.push(shared.jobs.len());
                    }
                    shared.jobs.push(job);
                } else if let Some((of, generic)) = shared.generic(file as u32, at, decl) {
                    match of {
                        GenericOf::Func(object) => drop(generics.insert(object, generic)),
                        GenericOf::Type(named) => methods.entry(named).or_default().push(generic),
                        GenericOf::Method(named, name) => drop(own.insert((named, name), generic)),
                    }
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
            let declared = shared.local_types(body).into_iter();
            shared
                .locals
                .extend(declared.map(|(id, ordinal)| (id, (at, ordinal))));
        }
        shared.instantiate(&generics, &methods, &own);
        shared.number_literals();
        // What each function does to its parameters and its variables,
        // which every caller of it reads.
        let facts: Vec<_> = (shared.jobs.par_iter())
            .map(|job| facts::of(&shared, job))
            .collect();
        for (job, facts) in shared.jobs.iter_mut().zip(facts) {
            if !job.symbol.is_empty() {
                shared.lent.insert(job.symbol.clone(), facts.lent.clone());
                shared
                    .worked
                    .insert(job.symbol.clone(), facts.worked.clone());
            }
            job.facts = facts;
        }
        // A literal that a function is lent is framed, which is known
        // now that every function has its facts.
        let mut facts: Vec<_> = (shared.jobs.iter_mut())
            .map(|job| std::mem::take(&mut job.facts))
            .collect();
        (facts.par_iter_mut())
            .zip(shared.jobs.par_iter())
            .for_each(|(facts, job)| facts.lend(&shared, job));
        for (job, facts) in shared.jobs.iter_mut().zip(facts) {
            job.facts = facts;
        }
        shared
    }

    /// Gives every function literal of the package its number: from 1
    /// in the order of the source, among those that are written
    /// directly in the same function or literal.
    fn number_literals(&mut self) {
        for (file, (tree, _)) in self.files.iter().enumerate() {
            let file = file as u32;
            // The declarations of the file that have functions, in the
            // order of the source: the instances of a generic function
            // are of one declaration.
            let mut jobs: Vec<(u32, u32, usize)> = (self.jobs.iter())
                .filter(|job| job.file == file)
                .map(|job| {
                    let span = tree.decl_nodes[job.decl].span;
                    (span.lo, span.hi, job.at)
                })
                .collect();
            jobs.sort_unstable();
            jobs.dedup();
            let mut literals: Vec<(u32, std::cmp::Reverse<u32>, ExprId)> = (tree.exprs.iter())
                .filter(|(_, expr)| matches!(expr.kind, ExprKind::FuncLit { .. }))
                .map(|(id, expr)| (expr.span.lo, std::cmp::Reverse(expr.span.hi), id))
                .collect();
            literals.sort_unstable();
            // The literals that are open where the next one starts: the
            // last of them is the one that it is written in.
            let mut open: Vec<(u32, ExprId)> = Vec::new();
            let mut count: FxHashMap<(usize, Option<ExprId>), u64> = FxHashMap::default();
            for (lo, std::cmp::Reverse(hi), id) in literals {
                while open.last().is_some_and(|&(end, _)| end <= lo) {
                    open.pop();
                }
                let before = jobs.partition_point(|&(start, _, _)| start <= lo);
                let around = before.checked_sub(1).map(|at| jobs[at]);
                let Some((_, _, at)) = around.filter(|&(_, end, _)| hi <= end) else {
                    continue;
                };
                let within = open.last().map(|&(_, within)| within);
                let ordinal = count.entry((at, within)).or_default();
                *ordinal += 1;
                let literal = Literal {
                    at,
                    within,
                    ordinal: *ordinal,
                };
                self.literals.insert((file, id), literal);
                open.push((hi, id));
            }
        }
    }

    /// What the function of a job is, for its symbol.
    fn entity(&self, named: JobName) -> Entity<'_> {
        match named {
            JobName::Object(object) => Entity::Object(self.package, object),
            JobName::Method(receiver, name) => Entity::Method { receiver, name },
            JobName::Init(number) => Entity::InitFunc(self.package, number),
            JobName::Instance(object, at) => {
                Entity::FuncInstance(self.package, object, &self.instances[at].args)
            }
            JobName::OwnMethod(receiver, name, at) => {
                let instance = &self.instances[at];
                Entity::MethodInstance {
                    receiver,
                    name,
                    args: &instance.args[instance.of_receiver..],
                }
            }
        }
    }

    /// Whether the function of the job is an instance of a generic
    /// function, or a method of an instance of a generic type.
    pub fn is_instance(&self, job: &Job) -> bool {
        job.made.is_some()
    }

    /// The types that a body declares, each with its number among those
    /// of its name there, from 0.
    fn local_types(&self, body: &Body) -> Vec<(NamedId, u64)> {
        let mut count: FxHashMap<Spur, u64> = FxHashMap::default();
        let mut declared = Vec::new();
        for local in &body.locals {
            if let (LocalKind::Type, CheckedType::Named { id, .. }) =
                (local.kind, &*self.types.get(local.ty))
            {
                let ordinal = count.entry(local.name).or_default();
                declared.push((*id, *ordinal));
                *ordinal += 1;
            }
        }
        declared
    }

    /// What the checker recorded for the body of the function of a job.
    pub fn body(&self, job: &Job) -> &Body {
        match job.made {
            Some(at) => &self.instances[at].body,
            None => &self.package.files[job.file as usize].bodies[job.at],
        }
    }

    /// The method of the declared type `holder`, or of the instance of
    /// a generic type, that the package declares with the name.
    pub fn method(&self, holder: CheckedTypeId, name: Spur) -> Option<&Method> {
        match &*self.types.get(holder) {
            CheckedType::Named { id, args } if args.is_empty() => self.methods.get(&(*id, name)),
            CheckedType::Named { .. } => self.instance_methods.get(&(holder, name)),
            _ => None,
        }
    }

    /// What the declaration makes if it is generic and has a body: a
    /// generic function of the package, or a method of the generic
    /// type, with its name and whether it takes the address of its
    /// receiver; or a method with type parameters of its own. Their
    /// instances are lowered.
    fn generic(&self, file: u32, at: usize, decl: DeclId) -> Option<(GenericOf, Generic)> {
        let made = |method| Generic {
            file,
            at,
            decl,
            method,
        };
        let (tree, _) = self.files[file as usize];
        let DeclKind::Func {
            is_macro: false,
            receiver,
            name,
            type_params,
            body: Some(_),
            ..
        } = &tree.decl_nodes[decl].kind
        else {
            return None;
        };
        if self.names.resolve(&name.name) == "_" {
            return None;
        }
        let Some(receiver) = receiver else {
            if type_params.is_empty() {
                return None;
            }
            let id = *self.package.scope.get(&name.name)?;
            let object = self.package.object(id);
            let function = matches!(object.kind, ObjKind::Func) && object.span == name.span;
            return function.then_some((GenericOf::Func(id), made(None)));
        };
        let body = &self.package.files[file as usize].bodies[at];
        let (named, pointer) = self.receiver(tree, body, receiver)?;
        let CheckedType::Named { id, .. } = &*self.types.get(named) else {
            return None;
        };
        let method = made(Some((name.name, pointer)));
        match (
            type_params.is_empty(),
            self.types.type_params(*id).is_empty(),
        ) {
            (false, _) => Some((GenericOf::Method(*id, name.name), method)),
            (true, false) => Some((GenericOf::Type(*id), method)),
            (true, true) => None,
        }
    }

    /// What the expression names with type arguments: a generic
    /// function of the package (`f`, `f[int]`), or a method with type
    /// parameters of its own (`x.m`, `x.m[int]`), with its selector.
    pub fn generic_named(
        &self,
        tree: &File,
        body: &Body,
        expr: ExprId,
    ) -> Option<(Used, Option<ExprId>)> {
        let mut named = Self::unparen(tree, expr);
        if let ExprKind::Index { x, .. } = tree.exprs[named].kind {
            named = Self::unparen(tree, x);
        }
        match (&tree.exprs[named].kind, body.uses.get(&named)) {
            (ExprKind::Name(_), Some(Ref::Object(object))) => Some((Used::Func(*object), None)),
            (ExprKind::Selector { sel, .. }, _) => match body.selections.get(&named) {
                Some(selection) if selection.kind == gopp_sema::SelectionKind::Method => {
                    Some((Used::Method(selection.holder, sel.name), Some(named)))
                }
                _ => None,
            },
            _ => None,
        }
    }

    /// The job of what is used with the type arguments.
    pub fn instance(&self, used: Used, args: &[CheckedTypeId]) -> Option<usize> {
        self.instance_jobs.get(&(used, args.to_vec())).copied()
    }

    /// What the code of the function of the job is as a value: the
    /// generic function and its type arguments, for an instance.
    pub fn job_value(&self, job: usize) -> Option<Entity<'_>> {
        match self.jobs[job].named {
            JobName::Instance(object, at) => {
                let args = &self.instances[at].args;
                Some(Entity::InstanceValue(self.package, object, args))
            }
            _ => None,
        }
    }

    /// The symbol and the type of the function of the job, for a call.
    pub fn job_called(&self, job: usize) -> (String, Option<CheckedTypeId>, CheckedTypeId) {
        let job = &self.jobs[job];
        (job.symbol.clone(), job.receiver, job.sig)
    }

    /// Gives each instance of a generic function that is used a job,
    /// and each method of each instance of a generic type that is
    /// named anywhere in a type: a value of the type may be asked for
    /// any of its methods where it is in an interface. Two uses whose
    /// type arguments make the same symbol name one instance. What a
    /// level of functions uses is found in the order of the source,
    /// the types in the order of their symbols, and the bodies are
    /// made side by side; then they are the next level.
    fn instantiate(
        &mut self,
        generics: &FxHashMap<ObjId, Generic>,
        methods: &FxHashMap<NamedId, Vec<Generic>>,
        own: &FxHashMap<(NamedId, Spur), Generic>,
    ) {
        if generics.is_empty() && methods.is_empty() && own.is_empty() {
            return;
        }
        let mut by_symbol: FxHashMap<String, usize> = FxHashMap::default();
        let mut seen: FxHashSet<CheckedTypeId> = FxHashSet::default();
        let mut level = 0..self.jobs.len();
        while !level.is_empty() {
            let mut found: Vec<Found> = Vec::new();
            let mut known: FxHashMap<(Used, Vec<CheckedTypeId>), usize> = FxHashMap::default();
            let mut of_types: Vec<((CheckedTypeId, Spur), usize, bool)> = Vec::new();
            let mut steps = 0;
            // A type argument may be a type that a function declares.
            let locals = |named: NamedId| self.local(named);
            let mut namer = Namer::new(self.types, self.names).with_locals(&locals);
            let mut types: Vec<(String, CheckedTypeId)> = Vec::new();
            for at in level {
                let job = &self.jobs[at];
                let (tree, _) = self.files[job.file as usize];
                let body = self.body(job);
                let mut uses: Vec<_> = (body.instances.iter())
                    .filter_map(|(&expr, instance)| {
                        let (used, _) = self.generic_named(tree, body, expr)?;
                        let span = tree.exprs[expr].span;
                        Some((span.lo, span.hi, used, instance))
                    })
                    .collect();
                uses.sort_unstable_by_key(|&(lo, hi, ..)| (lo, hi));
                steps += uses.len() as u64;
                for (_, _, used, instance) in uses {
                    let key = (used, instance.args.clone());
                    if self.instance_jobs.contains_key(&key) || known.contains_key(&key) {
                        continue;
                    }
                    // What is used, with the type arguments of its
                    // receiver before its own.
                    let (of, named, entity, of_receiver) = match used {
                        Used::Func(object) => {
                            let entity = Entity::FuncInstance(self.package, object, &instance.args);
                            (
                                generics.get(&object),
                                Named::Func(object),
                                entity,
                                Vec::new(),
                            )
                        }
                        Used::Method(receiver, name) => {
                            let CheckedType::Named { id, args } = &*self.types.get(receiver) else {
                                continue;
                            };
                            let entity = Entity::MethodInstance {
                                receiver,
                                name,
                                args: &instance.args,
                            };
                            let named = Named::OwnMethod(receiver, name);
                            (own.get(&(*id, name)), named, entity, args.to_vec())
                        }
                    };
                    let (Some(&of), Ok(symbol)) = (of, symbol_of(&mut namer, entity)) else {
                        continue;
                    };
                    let job = *by_symbol.entry(symbol.clone()).or_insert_with(|| {
                        let mut args = of_receiver;
                        let of_receiver = args.len();
                        args.extend(&instance.args);
                        found.push(Found {
                            of,
                            named,
                            args,
                            of_receiver,
                            sig: instance.ty,
                            symbol,
                        });
                        self.jobs.len() + found.len() - 1
                    });
                    known.insert(key, job);
                }
                // The instances of generic types with methods that the
                // types of the body are written with.
                let typed = body.types.values().map(|typed| typed.ty);
                let mut rest: Vec<CheckedTypeId> = typed
                    .chain(body.locals.iter().map(|local| local.ty))
                    .collect();
                while let Some(ty) = rest.pop() {
                    if !seen.insert(ty) {
                        continue;
                    }
                    steps += 1;
                    let kind = self.types.get(ty);
                    kind.each_part(&mut |part| rest.push(part));
                    if let CheckedType::Named { id, args } = &*kind {
                        rest.push(self.types.underlying(ty));
                        let symbol = (!args.is_empty() && methods.contains_key(id))
                            .then(|| symbol_of(&mut namer, Entity::TypeInfo(ty)).ok());
                        types.extend(symbol.flatten().map(|symbol| (symbol, ty)));
                    }
                }
            }
            types.sort_unstable();
            for (_, ty) in types {
                let CheckedType::Named { id, args } = &*self.types.get(ty) else {
                    continue;
                };
                for &of in methods.get(id).into_iter().flatten() {
                    let Some((name, pointer)) = of.method else {
                        continue;
                    };
                    let found_method = self.types.lookup(ty, name, self.path, self.names, false);
                    let sig = match found_method.map(|found| found.member) {
                        Ok(gopp_sema::types::Member::Method { sig, .. }) => sig,
                        _ => continue,
                    };
                    let entity = Entity::Method { receiver: ty, name };
                    let Ok(symbol) = symbol_of(&mut namer, entity) else {
                        continue;
                    };
                    steps += 1;
                    let job = *by_symbol.entry(symbol.clone()).or_insert_with(|| {
                        found.push(Found {
                            of,
                            named: Named::Method(ty, name),
                            args: args.to_vec(),
                            of_receiver: args.len(),
                            sig,
                            symbol,
                        });
                        self.jobs.len() + found.len() - 1
                    });
                    of_types.push(((ty, name), job, pointer));
                }
            }
            self.instance_jobs.extend(known);
            self.found_steps += steps;
            let bodies: Vec<Body> = (found.par_iter())
                .map(|found| {
                    let Generic { file, at, .. } = found.of;
                    let generic = &self.package.files[file as usize].bodies[at];
                    let dialect = self.files[file as usize].1.dialect();
                    let written = gopp_sema::Written {
                        types: self.types,
                        names: self.names,
                        package: self.path,
                        own_rules: dialect == gopp_span::Dialect::Gopp,
                    };
                    generic.instantiated(written, &found.args)
                })
                .collect();
            let start = self.jobs.len();
            for (found, body) in found.into_iter().zip(bodies) {
                self.found_steps += body.types.len() as u64;
                let Generic { file, at, decl, .. } = found.of;
                let (tree, _) = self.files[file as usize];
                let span = tree.decl_nodes[decl].span;
                let made = self.instances.len();
                let receiver = |ty| match found.of.method {
                    Some((_, true)) => self.types.pointer(ty),
                    _ => ty,
                };
                let (named, receiver) = match found.named {
                    Named::Func(object) => (JobName::Instance(object, made), None),
                    Named::Method(ty, name) => (JobName::Method(ty, name), Some(receiver(ty))),
                    Named::OwnMethod(ty, name) => {
                        (JobName::OwnMethod(ty, name, made), Some(receiver(ty)))
                    }
                };
                self.instances.push(Instantiated {
                    args: found.args,
                    of_receiver: found.of_receiver,
                    body,
                });
                self.jobs.push(Job {
                    file,
                    at,
                    decl,
                    named,
                    symbol: found.symbol,
                    receiver,
                    sig: found.sig,
                    source: (span.hi - span.lo) as usize,
                    facts: facts::Facts::default(),
                    made: Some(made),
                });
            }
            // The types that an instance declares are its own.
            for at in start..self.jobs.len() {
                let declared = self.local_types(self.body(&self.jobs[at])).into_iter();
                self.locals
                    .extend(declared.map(|(id, ordinal)| (id, (at, ordinal))));
            }
            for (key, job, _) in of_types {
                let job = &self.jobs[job];
                let Some(receiver) = job.receiver else {
                    continue;
                };
                let dialect = self.files[job.file as usize].1.dialect();
                let method = Method {
                    symbol: job.symbol.clone(),
                    receiver,
                    sig: job.sig,
                    gopp: dialect == gopp_span::Dialect::Gopp,
                };
                self.instance_methods.insert(key, method);
            }
            level = start..self.jobs.len();
        }
    }

    /// The function that declares the type, and which of the types of
    /// its name there the type is.
    fn local(&self, named: NamedId) -> Option<(Entity<'_>, u64)> {
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
            made: None,
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
                // The receiver of a method of a generic type names
                // the type parameters.
                ExprKind::Index { x, .. } => base = x,
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
            // The literals of the function, and those of the literals.
            while let Some((job, literal)) = memo.literals.pop() {
                func::lower_literal(self, &mut module, &mut memo, job, literal);
            }
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
                    Made::Value(of) => {
                        func::lower_value(self, &mut module, &mut memo, &symbol, (&of, ty));
                    }
                    Made::MethodValue(_) | Made::MethodExpr { .. } => {
                        func::lower_bound(self, &mut module, &mut memo, &symbol, (kind, ty));
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
