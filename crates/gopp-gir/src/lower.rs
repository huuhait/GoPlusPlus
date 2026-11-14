//! From a checked package to its module (gir.md §1, §3, §15).
//!
//! The bodies are lowered side by side, in runs that each fill a module
//! of their own; `merge` then makes one module of them, in which every
//! type, string and function has the place that the order of the source
//! gives it. So the module is the same for any number of threads.

mod assign;
mod cabi;
mod closure;
mod equal;
mod expr;
mod facts;
mod func;
mod glue;
mod iface;
mod map;
mod merge;
mod operator;
mod option;
mod panic;
mod ranged;
mod slice;
mod stmt;
mod table;
mod types;

#[cfg(test)]
pub(crate) mod tests;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_mangle::{Entity, Locals, Namer, symbol_of};
use gopp_sema::types::{NamedId, Type as CheckedType, TypeId as CheckedTypeId, Types};
use gopp_sema::{Body, LocalKind, ObjId, ObjKind, Package, Ref};
use gopp_span::{SourceFile, Span};
use gopp_syntax::ast::{
    DeclId, DeclKind, ExprId, ExprKind, File, Param, StmtId, StmtKind, UnaryOp,
};
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
    /// The packages that the package imports, and those that they
    /// import: an instance of generic code of theirs is made here.
    pub imports: &'a [Imported<'a>],
    pub types: &'a Types,
    pub names: &'a ThreadedRodeo,
    /// A panic ends the program at once (`-panic=abort`, PAN-7): no
    /// deferred call is made for it and nothing is unwound.
    pub aborts: bool,
}

/// A package that another one imports, with its files in the order in
/// which they were checked.
#[derive(Clone, Copy)]
pub struct Imported<'a> {
    pub package: &'a Package,
    pub files: &'a [(&'a File, &'a SourceFile)],
}

/// The least source that one run takes: lowering it costs more than
/// starting a run and merging what it made.
const SOURCE_EACH: usize = 256 * 1024;

/// What the callers of the functions of packages know of them, by the
/// symbol of each function: which parameters it does not own, so that a
/// caller lends its arguments to those, and through which it calls
/// methods; and the functions that call `recover` themselves. A package
/// that imports others is lowered with theirs.
#[derive(Clone, Debug, Default)]
pub struct Summaries {
    lent: FxHashMap<String, Vec<bool>>,
    worked: FxHashMap<String, Vec<bool>>,
    recovers: FxHashSet<String>,
}

impl Summaries {
    /// Adds what is known of the functions of more packages.
    pub fn extend(&mut self, more: &Summaries) {
        let pairs = |map: &FxHashMap<String, Vec<bool>>| {
            let cloned = map
                .iter()
                .map(|(symbol, facts)| (symbol.clone(), facts.clone()));
            cloned.collect::<Vec<_>>()
        };
        self.lent.extend(pairs(&more.lent));
        self.worked.extend(pairs(&more.worked));
        self.recovers.extend(more.recovers.iter().cloned());
    }
}

/// The module of a package that the checker accepted. What the lowering
/// does not support yet is reported to `sink` as `E0439`, each place
/// once, and has a zero of its type in the module; the module is
/// verified, and a rule that it breaks is reported as `E0440`.
pub fn lower_package(checked: &Checked<'_>, layout: TargetLayout, sink: &Sink) -> Module {
    lower_with(checked, &Summaries::default(), layout, sink).0
}

/// `lower_package` for a package that imports others, with what is
/// known of their functions; and what the packages that import this one
/// know of its functions.
pub fn lower_with(
    checked: &Checked<'_>,
    imported: &Summaries,
    layout: TargetLayout,
    sink: &Sink,
) -> (Module, Summaries) {
    let limits = Limits::OF_A_PACKAGE;
    let (module, diagnostics, _, own) = lower_all(checked, imported, layout, SOURCE_EACH, limits);
    for diagnostic in diagnostics {
        sink.push(diagnostic);
    }
    (module, own)
}

/// Reports what the instances of the generic code of a package that
/// the checker accepted are refused for (`E0449`), as `lower_package`
/// does: for `gopp check`, which lowers nothing.
pub fn check_instances(checked: &Checked<'_>, sink: &Sink) {
    let shared = Shared::found(checked, TargetLayout::new(64), Limits::OF_A_PACKAGE);
    for diagnostic in shared.refused {
        sink.push(diagnostic);
    }
}

/// The bounds of the instances of a package (GEN-5).
#[derive(Clone, Copy)]
pub(crate) struct Limits {
    /// How many levels a type argument that generic code builds nests.
    pub height: u32,
    /// How many parts it has as a tree.
    pub parts: u64,
    /// How many instances a package has, and how many for each node of
    /// its syntax if that is more.
    pub instances: (u64, u64),
    /// How many nodes of syntax the package and its instances are, and
    /// how many for each node of the package if that is more.
    pub code: (u64, u64),
}

impl Limits {
    pub const OF_A_PACKAGE: Self = Self {
        height: 48,
        parts: 4096,
        instances: (100_000, 8),
        code: (4_000_000, 64),
    };
}

/// `lower_package`, with what it reports and the steps that it took.
/// A run takes `source_each` bytes of source or more.
#[cfg(test)]
pub(crate) fn lower_counted(
    checked: &Checked<'_>,
    layout: TargetLayout,
    source_each: usize,
) -> (Module, Vec<Diagnostic>, u64) {
    lower_bounded(checked, layout, source_each, Limits::OF_A_PACKAGE)
}

/// `lower_counted`, with the bounds of the instances.
#[cfg(test)]
pub(crate) fn lower_bounded(
    checked: &Checked<'_>,
    layout: TargetLayout,
    source_each: usize,
    limits: Limits,
) -> (Module, Vec<Diagnostic>, u64) {
    let none = Summaries::default();
    let (module, diagnostics, steps, _) = lower_all(checked, &none, layout, source_each, limits);
    (module, diagnostics, steps)
}

/// `lower_bounded` with what is known of the functions of the packages
/// that are imported, and what is found of those of this one.
fn lower_all(
    checked: &Checked<'_>,
    imported: &Summaries,
    layout: TargetLayout,
    source_each: usize,
    limits: Limits,
) -> (Module, Vec<Diagnostic>, u64, Summaries) {
    let mut shared = Shared::found(checked, layout, limits);
    // Nothing is lowered of a package whose instances are refused.
    if !shared.refused.is_empty() {
        let refused = std::mem::take(&mut shared.refused);
        return (
            shared.module(),
            refused,
            shared.found_steps,
            Summaries::default(),
        );
    }
    shared.lent.clone_from(&imported.lent);
    shared.worked.clone_from(&imported.worked);
    shared.recovers.clone_from(&imported.recovers);
    shared.facts();
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
    // What is known of the functions of this package only.
    let mut own = Summaries::default();
    for job in shared.jobs.iter().filter(|job| !job.symbol.is_empty()) {
        if let Some(lent) = shared.lent.get(&job.symbol) {
            own.lent.insert(job.symbol.clone(), lent.clone());
        }
        if let Some(worked) = shared.worked.get(&job.symbol) {
            own.worked.insert(job.symbol.clone(), worked.clone());
        }
        if shared.recovers.contains(&job.symbol) {
            own.recovers.insert(job.symbol.clone());
        }
    }
    (module, diagnostics, steps, own)
}

/// How many of the uses that led to a refused instance are named.
const MAX_LED: usize = 16;

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
    /// For an instance: the function that used it first, and where.
    by: Option<(usize, Span)>,
}

/// What a function with a body is, for its symbol.
#[derive(Clone, Copy)]
enum JobName {
    Object(ObjId),
    /// A method of the declared type.
    Method(CheckedTypeId, Spur),
    /// An `@Operator` method of the declared type: which of them, in
    /// the order of their declarations. Several may have one name.
    Operator(CheckedTypeId, u32),
    /// An `init` function, and its number once the files are counted.
    Init(u64),
    /// A generic function with type arguments: the package that
    /// declares it and the function, and the instance of the package
    /// that has the arguments.
    Instance(u32, ObjId, usize),
    /// A method with type parameters of its own, of the type, with type
    /// arguments: the instance of the package has them last.
    OwnMethod(CheckedTypeId, Spur, usize),
    /// What gives variables of the package their first value: which
    /// step of the start of the program (`Package::init_order`).
    Initializer(usize),
}

/// A generic function, or a method with type parameters of its own of
/// a type, as a use names it.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum Used {
    /// A function of the package with the number: 0 is the package that
    /// is lowered.
    Func(u32, ObjId),
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
    /// A function of the package with the number.
    Func(u32, ObjId),
    Type(NamedId),
    Method(NamedId, Spur),
}

/// An instance that a level of functions uses first.
struct Found {
    of: Generic,
    /// The function that uses it, and where.
    by: (usize, Span),
    named: Named,
    args: Vec<CheckedTypeId>,
    of_receiver: usize,
    sig: CheckedTypeId,
    symbol: String,
}

/// What an instance is an instance of.
#[derive(Clone, Copy)]
enum Named {
    Func(u32, ObjId),
    /// A method of the instance of a generic type.
    Method(CheckedTypeId, Spur),
    /// An `@Operator` method of the instance of a generic type: which
    /// of them, in the order of their declarations.
    Operator(CheckedTypeId, u32),
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
}

/// What every run reads and none writes.
pub(crate) struct Shared<'a> {
    pub package: &'a Package,
    /// The files of the package, then those of the packages that it
    /// imports.
    pub files: Vec<(&'a File, &'a SourceFile)>,
    /// How many of the files are of the package itself.
    own_files: usize,
    /// The package, then those that it imports.
    packages: Vec<&'a Package>,
    /// For each file: which of the packages it is of, and which of the
    /// files of that package.
    homes: Vec<(u32, u32)>,
    pub types: &'a Types,
    pub names: &'a ThreadedRodeo,
    pub layout: TargetLayout,
    /// A panic ends the program at once.
    pub aborts: bool,
    /// The path of the package, as a name.
    pub path: Spur,
    /// The name `_`.
    pub blank: Spur,
    /// The name `base`.
    pub base: Spur,
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
    limits: Limits,
    /// What the instances of the package are refused for.
    refused: Vec<Diagnostic>,
    pub methods: FxHashMap<(NamedId, Spur), Method>,
    /// The `@Operator` methods of the types of the package and of
    /// those that it imports, each by its place among those of its
    /// type; and those of each type as the checker has them.
    operators: FxHashMap<(NamedId, u32), Method>,
    /// The `@Operator` methods of the instances of generic types.
    instance_operators: FxHashMap<(CheckedTypeId, u32), Method>,
    operator_decls: FxHashMap<CheckedTypeId, Vec<gopp_sema::types::DeclaredMethod>>,
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
    /// The `range` statements of the package, each by its file: the body
    /// of one over a function is a function, with a number after those
    /// of the literals that are written in the same function.
    ranges: FxHashMap<(u32, StmtId), Literal>,
    /// For each declaration of a file with functions: its `range`
    /// statements, its statements with labels and its jumps to labels,
    /// in the order of the source.
    jumps: FxHashMap<(u32, usize), Vec<StmtId>>,
    /// For the symbol of each function of the package: which of its
    /// parameters it does not own, the receiver first. A caller lends
    /// its arguments to those.
    pub lent: FxHashMap<String, Vec<bool>>,
    /// For the symbol of each function of the package: which of its
    /// parameters it calls methods through. A caller lends those a
    /// value that nothing else reads.
    pub worked: FxHashMap<String, Vec<bool>>,
    /// The symbols of the functions that call `recover` themselves.
    pub recovers: FxHashSet<String>,
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
    /// An `@Operator` method of the type: which of them.
    Operator(CheckedTypeId, u32),
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
    /// The function calls `recover` itself.
    pub recovers: bool,
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
    /// The same for a function of C, which is called as C calls it.
    ValueOfC(String),
    /// The function of C that calls the function with the symbol, for C
    /// code that is given the function.
    CThunk(String),
    /// The code of a method value: the method `name` of a declared
    /// type or of an interface type, with its receiver in the
    /// environment. A method with type parameters of its own is the
    /// instance of the job.
    MethodValue(Spur, Option<usize>),
    /// What a type table has for the method `name` of a type: the
    /// method with a receiver that is `by` bytes before its own.
    Adjusted { name: Spur, by: i64 },
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
    /// The bodies of the loops over functions that the functions of the
    /// run have, which are lowered as the literals are; and what the
    /// function with the loop says of each.
    pub ranges: Vec<(&'a Job, StmtId)>,
    pub ranged: FxHashMap<(*const Job, StmtId), std::rc::Rc<ranged::Ranged>>,
    /// The places where the body of a job names its variables, in the
    /// order of the source, and its receiver.
    pub named: FxHashMap<*const Job, std::rc::Rc<ranged::Named>>,
    /// The functions whose deferred calls are lowered when the bodies
    /// of their loops over functions are, and what the `defer`
    /// statements of those bodies call, by the function or the literal.
    pub later: Vec<func::Later<'a>>,
    pub late: FxHashMap<(*const Job, Option<ExprId>), Vec<func::Late>>,
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
    /// The types of the `Unique` handles of the module, each with the
    /// type of what it owns: a pointer to a box of its own, which holds
    /// the value first.
    pub uniques: FxHashMap<TypeId, TypeId>,
    /// The same for the `Shared` handles and for the `Weak` handles:
    /// the box of such a handle has the counts of its handles in front
    /// of it, which the runtime keeps.
    pub shareds: FxHashMap<TypeId, TypeId>,
    pub weaks: FxHashMap<TypeId, TypeId>,
    /// The structs of the types with parents or with a type table,
    /// each with the number of the parts that come before its fields:
    /// the address of its table if the type has one and its first
    /// parent has none, then the parts of its parents in the order of
    /// the `@Extends` (TYP-22). `tabled` has those that start with a
    /// table of their own.
    pub leads: FxHashMap<TypeId, u32>,
    pub tabled: rustc_hash::FxHashSet<TypeId>,
    pub funcs: FxHashMap<String, crate::FuncId>,
    pub globals: FxHashMap<String, GlobalId>,
    /// The global of each variable that the run has named.
    pub global_keys: FxHashMap<(u32, CalleeKey), GlobalId>,
    /// The type of GIR for each type that an expression or a variable
    /// has: the empty type for a constant without a type.
    pub expr_types: FxHashMap<CheckedTypeId, Result<TypeId, &'static str>>,
    /// What is known of each function that the run calls.
    pub callees: FxHashMap<(u32, CalleeKey), Result<Callee, &'static str>>,
    /// The text of the names that the run has asked for.
    pub names: FxHashMap<Spur, Box<str>>,
    /// Which field of a struct has a name.
    pub fields: FxHashMap<(TypeId, Spur), Option<usize>>,
    /// The type of the checker of each struct of the module whose
    /// values start with the addresses of tables.
    pub with_tables: FxHashMap<TypeId, CheckedTypeId>,
    /// Whether a value of a type of the module holds a value with
    /// tables, and the global that a new value of such a type starts
    /// as: none for a type without one.
    pub holds_tables: FxHashMap<TypeId, bool>,
    pub blanks: FxHashMap<TypeId, Option<GlobalId>>,
    /// Which types of the module cross to C.
    pub c_rules: crate::CTypeRules,
    /// For each type that was asked: its first part that C has no type
    /// for.
    pub c_parts: FxHashMap<CheckedTypeId, Option<CheckedTypeId>>,
    pub diagnostics: Vec<Diagnostic>,
    pub steps: u64,
}

impl<'a> Memo<'a> {
    /// What a `Unique` or a `Shared` handle of the type leads to: a
    /// field or a method of it is reached through the handle.
    pub fn held(&self, handle: TypeId) -> Option<TypeId> {
        let held = self.uniques.get(&handle);
        held.or_else(|| self.shareds.get(&handle)).copied()
    }

    fn new(shared: &'a Shared<'_>, locals: &'a Locals<'a>) -> Self {
        Self {
            namer: Namer::new(shared.types, shared.names).with_locals(locals),
            types: FxHashMap::default(),
            map: None,
            func_types: rustc_hash::FxHashSet::default(),
            literals: Vec::new(),
            queued: rustc_hash::FxHashSet::default(),
            ranges: Vec::new(),
            ranged: FxHashMap::default(),
            named: FxHashMap::default(),
            later: Vec::new(),
            late: FxHashMap::default(),
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
            uniques: FxHashMap::default(),
            shareds: FxHashMap::default(),
            weaks: FxHashMap::default(),
            leads: FxHashMap::default(),
            tabled: rustc_hash::FxHashSet::default(),
            funcs: FxHashMap::default(),
            globals: FxHashMap::default(),
            global_keys: FxHashMap::default(),
            expr_types: FxHashMap::default(),
            callees: FxHashMap::default(),
            names: FxHashMap::default(),
            fields: FxHashMap::default(),
            with_tables: FxHashMap::default(),
            holds_tables: FxHashMap::default(),
            blanks: FxHashMap::default(),
            c_rules: crate::CTypeRules::new(Some(shared.layout)),
            c_parts: FxHashMap::default(),
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
            None => shared.home_node(&mut self.namer, job)?,
        };
        self.namer.closure(within, literal.ordinal).ok()
    }

    /// The symbol of the body of the loop over a function that is
    /// written in the function of the job: that of a literal there,
    /// with its number.
    pub fn range_body(&mut self, shared: &Shared<'_>, job: &Job, stmt: StmtId) -> Option<String> {
        let body = shared.ranges.get(&(job.file, stmt))?;
        if body.at != job.at {
            return None;
        }
        let within = match body.within {
            Some(within) => self.literal_node(shared, job, within)?,
            None => shared.home_node(&mut self.namer, job)?,
        };
        let node = self.namer.closure(within, body.ordinal).ok()?;
        Some(self.namer.symbol(node))
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
            None => shared.job_node(&mut self.namer, job)?,
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

/// The packages that the package imports, and those that they import,
/// each once.
fn imported(package: &Package) -> Vec<&Package> {
    let mut found: Vec<&Package> = Vec::new();
    let mut seen: FxHashSet<&str> = FxHashSet::default();
    let mut next = vec![package];
    while let Some(package) = next.pop() {
        for object in &package.objects {
            let ObjKind::Import { package: of, .. } = &object.kind else {
                continue;
            };
            if seen.insert(&of.path) {
                found.push(of);
                next.push(of);
            }
        }
    }
    found
}

impl<'a> Shared<'a> {
    /// The package that the file is of: what its bodies name is there.
    pub fn home(&self, file: u32) -> &'a Package {
        self.packages[self.homes[file as usize].0 as usize]
    }

    /// The number of the package that the file is of.
    pub fn package_of(&self, file: u32) -> u32 {
        self.homes[file as usize].0
    }

    /// The path of the package that the file is of: a name that is not
    /// exported is one of that package.
    fn path_of(&self, file: u32) -> Spur {
        self.names.get_or_intern(&self.home(file).path)
    }

    /// What the checker recorded for the file.
    fn info(&self, file: u32) -> &'a gopp_sema::FileInfo {
        let (package, at) = self.homes[file as usize];
        &self.packages[package as usize].files[at as usize]
    }

    /// What every run reads, without what each function does to its
    /// parameters and its variables (`facts`).
    fn found(checked: &'a Checked<'a>, layout: TargetLayout, limits: Limits) -> Self {
        let mut files = checked.files.to_vec();
        let mut packages = vec![checked.package];
        let mut homes: Vec<(u32, u32)> = (0..files.len() as u32).map(|at| (0, at)).collect();
        for imported in checked.imports {
            let package = packages.len() as u32;
            packages.push(imported.package);
            files.extend(imported.files);
            homes.extend((0..imported.files.len() as u32).map(|at| (package, at)));
        }
        let mut shared = Self {
            package: checked.package,
            files,
            own_files: checked.files.len(),
            packages,
            homes,
            types: checked.types,
            names: checked.names,
            layout,
            aborts: checked.aborts,
            path: checked.names.get_or_intern(&checked.package.path),
            blank: checked.names.get_or_intern_static("_"),
            base: checked.names.get_or_intern_static("base"),
            drop: checked.names.get_or_intern_static("drop"),
            clone: checked.names.get_or_intern_static("Clone"),
            error: checked.names.get_or_intern_static("Error"),
            string: checked.names.get_or_intern_static("String"),
            jobs: Vec::new(),
            instances: Vec::new(),
            instance_jobs: FxHashMap::default(),
            instance_methods: FxHashMap::default(),
            found_steps: 0,
            limits,
            refused: Vec::new(),
            methods: FxHashMap::default(),
            operators: FxHashMap::default(),
            instance_operators: FxHashMap::default(),
            operator_decls: FxHashMap::default(),
            locals: FxHashMap::default(),
            no_body: Body::default(),
            no_facts: facts::Facts::default(),
            literals: FxHashMap::default(),
            ranges: FxHashMap::default(),
            jumps: FxHashMap::default(),
            lent: FxHashMap::default(),
            worked: FxHashMap::default(),
            recovers: FxHashSet::default(),
        };
        // The `init` functions count in the order of the names of their
        // files, then of the source.
        let mut inits: Vec<usize> = Vec::new();
        let mut generics: FxHashMap<(u32, ObjId), Generic> = FxHashMap::default();
        let mut methods: FxHashMap<NamedId, Vec<Generic>> = FxHashMap::default();
        let mut own: FxHashMap<(NamedId, Spur), Generic> = FxHashMap::default();
        let files = shared.files.clone();
        for (file, (tree, _)) in files.iter().enumerate() {
            // Of a package that is imported only the generic code is
            // of use here: its unit has the rest.
            let is_own = file < shared.own_files;
            for (at, &decl) in tree.decls.iter().enumerate() {
                let job = is_own.then(|| shared.job(file as u32, at, decl)).flatten();
                if let Some(job) = job {
                    if matches!(job.named, JobName::Init(_)) {
                        inits.push(shared.jobs.len());
                    }
                    if let JobName::Operator(named, _) = job.named
                        && let CheckedType::Named { id, .. } = &*shared.types.get(named)
                    {
                        let declared = shared.types.declared_operators(*id);
                        shared.operator_decls.insert(named, declared);
                    }
                    shared.jobs.push(job);
                } else if let Some((of, generic)) = shared.generic(file as u32, at, decl) {
                    match of {
                        GenericOf::Func(package, object) => {
                            generics.insert((package, object), generic);
                        }
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
        // A first value of a variable is given by a function of its
        // own, in the order that the checker found for the start. The
        // declarations of each file, by where they are, say which one
        // a value is written in.
        let mut placed: FxHashMap<u32, Vec<(u32, u32, usize)>> = FxHashMap::default();
        for (step, first) in shared.package.init_order.iter().enumerate() {
            let (tree, _) = files[first.file as usize];
            let decls = placed.entry(first.file).or_insert_with(|| {
                let spans = tree.decls.iter().enumerate().map(|(at, &decl)| {
                    let span = tree.decl_nodes[decl].span;
                    (span.lo, span.hi, at)
                });
                let mut decls: Vec<_> = spans.collect();
                decls.sort_unstable();
                decls
            });
            let span = tree.exprs[first.value].span;
            let before = decls.partition_point(|&(start, _, _)| start <= span.lo);
            let around = before.checked_sub(1).map(|at| decls[at]);
            if let Some((lo, hi, at)) = around.filter(|&(_, end, _)| span.hi <= end) {
                shared.jobs.push(Job {
                    file: first.file,
                    at,
                    decl: tree.decls[at],
                    named: JobName::Initializer(step),
                    symbol: String::new(),
                    receiver: None,
                    sig: shared.types.func(&[], &[], false),
                    source: (hi - lo) as usize,
                    facts: facts::Facts::default(),
                    made: None,
                    by: None,
                });
            }
        }
        let mut namer = Namer::new(shared.types, shared.names);
        let symbols: Vec<String> = (shared.jobs.iter())
            .map(|job| {
                let node = shared.job_node(&mut namer, job);
                node.map_or(String::new(), |node| namer.symbol(node))
            })
            .collect();
        for (at, symbol) in symbols.into_iter().enumerate() {
            let job = &mut shared.jobs[at];
            job.symbol = symbol;
            if let (JobName::Method(named, name), Some(receiver)) = (job.named, job.receiver)
                && let CheckedType::Named { id, .. } = &*shared.types.get(named)
            {
                let method = Method {
                    symbol: job.symbol.clone(),
                    receiver,
                    sig: job.sig,
                };
                shared.methods.insert((*id, name), method);
            }
            if let (JobName::Operator(named, which), Some(receiver)) = (job.named, job.receiver)
                && let CheckedType::Named { id, .. } = &*shared.types.get(named)
            {
                let method = |symbol: &str| Method {
                    symbol: symbol.to_owned(),
                    receiver,
                    sig: job.sig,
                };
                shared.operators.insert((*id, which), method(&job.symbol));
                // The one `@Operator` method of its name is called by
                // the name like any other method.
                let declared = &shared.operator_decls[&named];
                let name = declared[which as usize].method.name;
                let alone = declared.iter().filter(|other| other.method.name == name);
                if alone.count() == 1 {
                    shared.methods.insert((*id, name), method(&job.symbol));
                }
            }
            let (file, body) = (job.file, job.at);
            let body = &shared.info(file).bodies[body];
            let declared = shared.local_types(body).into_iter();
            shared
                .locals
                .extend(declared.map(|(id, ordinal)| (id, (at, ordinal))));
        }
        // The methods of the types of the packages that are imported:
        // the units of those packages have them.
        for package in imported(shared.package) {
            for object in &package.objects {
                let CheckedType::Named { id, args } = &*shared.types.get(object.ty) else {
                    continue;
                };
                if !matches!(object.kind, ObjKind::Type) || !args.is_empty() {
                    continue;
                }
                for declared in shared.types.declared_methods(*id) {
                    let name = declared.method.name;
                    let entity = Entity::Method {
                        receiver: object.ty,
                        name,
                    };
                    let generic = !declared.receiver_params.is_empty();
                    let Some(node) = namer.entity(entity).ok().filter(|_| !generic) else {
                        continue;
                    };
                    let receiver = match declared.pointer_receiver {
                        true => shared.types.pointer(object.ty),
                        false => object.ty,
                    };
                    let method = Method {
                        symbol: namer.symbol(node),
                        receiver,
                        sig: declared.method.sig,
                    };
                    shared.methods.entry((*id, name)).or_insert(method);
                }
                let operators = shared.types.declared_operators(*id);
                for (which, declared) in operators.iter().enumerate() {
                    let entity = Entity::Operator {
                        receiver: object.ty,
                        method: declared,
                    };
                    let Ok(node) = namer.entity(entity) else {
                        continue;
                    };
                    let receiver = match declared.pointer_receiver {
                        true => shared.types.pointer(object.ty),
                        false => object.ty,
                    };
                    let symbol = namer.symbol(node);
                    let method = |symbol: &str| Method {
                        symbol: symbol.to_owned(),
                        receiver,
                        sig: declared.method.sig,
                    };
                    let key = (*id, which as u32);
                    shared.operators.entry(key).or_insert(method(&symbol));
                    let name = declared.method.name;
                    let alone = operators.iter().filter(|other| other.method.name == name);
                    if alone.count() == 1 {
                        shared.methods.entry((*id, name)).or_insert(method(&symbol));
                    }
                }
            }
        }
        shared.instantiate(&generics, &methods, &own);
        shared.number_literals();
        shared
    }

    /// What each function does to its parameters and its variables,
    /// which every caller of it reads.
    fn facts(&mut self) {
        let shared = &*self;
        let facts: Vec<_> = (shared.jobs.par_iter())
            .map(|job| facts::of(shared, job))
            .collect();
        for (job, facts) in self.jobs.iter_mut().zip(facts) {
            if !job.symbol.is_empty() {
                self.lent.insert(job.symbol.clone(), facts.lent.clone());
                self.worked.insert(job.symbol.clone(), facts.worked.clone());
                if facts.recovers {
                    self.recovers.insert(job.symbol.clone());
                }
            }
            job.facts = facts;
        }
        // A literal that a function is lent is framed, which is known
        // now that every function has its facts.
        let mut facts: Vec<_> = (self.jobs.iter_mut())
            .map(|job| std::mem::take(&mut job.facts))
            .collect();
        let shared = &*self;
        (facts.par_iter_mut())
            .zip(shared.jobs.par_iter())
            .for_each(|(facts, job)| facts.lend(shared, job));
        for (job, facts) in self.jobs.iter_mut().zip(facts) {
            job.facts = facts;
        }
    }

    /// Gives every function literal of the package its number: from 1
    /// in the order of the source, among those that are written
    /// directly in the same function or literal.
    fn number_literals(&mut self) {
        // A file of another package is read only if an instance is
        // made of its generic code.
        let mut has_jobs = vec![false; self.files.len()];
        for job in &self.jobs {
            has_jobs[job.file as usize] = true;
        }
        for (file, (tree, _)) in self.files.iter().enumerate() {
            if !has_jobs[file] {
                continue;
            }
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
            // The `range` statements, after the literals: the literal
            // that each is written in is the last that is open there.
            let mut literals: Vec<(u32, u32, ExprId)> = (tree.exprs.iter())
                .filter(|(id, _)| self.literals.contains_key(&(file, *id)))
                .map(|(id, expr)| (expr.span.lo, expr.span.hi, id))
                .collect();
            literals.sort_unstable_by_key(|&(lo, hi, _)| (lo, std::cmp::Reverse(hi)));
            let mut ranges: Vec<(u32, u32, StmtId)> = (tree.stmts.iter())
                .filter(|(_, stmt)| matches!(stmt.kind, StmtKind::Range { .. }))
                .map(|(id, stmt)| (stmt.span.lo, stmt.span.hi, id))
                .collect();
            ranges.sort_unstable();
            let (mut next, mut open) = (0, Vec::new());
            for (lo, hi, id) in ranges {
                while let Some(&(start, end, literal)) = literals.get(next).filter(|l| l.0 <= lo) {
                    while open.last().is_some_and(|&(end, _)| end <= start) {
                        open.pop();
                    }
                    open.push((end, literal));
                    next += 1;
                }
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
                let body = Literal {
                    at,
                    within,
                    ordinal: *ordinal,
                };
                self.ranges.insert((file, id), body);
            }
            // What a loop over a function is asked for: the loops, the
            // labels and the jumps to labels of each declaration.
            let mut asked: Vec<(u32, u32, StmtId)> = (tree.stmts.iter())
                .filter(|(_, stmt)| {
                    let jump = matches!(stmt.kind, StmtKind::Branch { label: Some(_), .. });
                    jump || matches!(stmt.kind, StmtKind::Range { .. } | StmtKind::Labeled { .. })
                })
                .map(|(id, stmt)| (stmt.span.lo, stmt.span.hi, id))
                .collect();
            asked.sort_unstable();
            for (lo, hi, id) in asked {
                let before = jobs.partition_point(|&(start, _, _)| start <= lo);
                let around = before.checked_sub(1).map(|at| jobs[at]);
                if let Some((_, _, at)) = around.filter(|&(_, end, _)| hi <= end) {
                    self.jumps.entry((file, at)).or_default().push(id);
                }
            }
        }
    }

    /// What the function of a job is, for its symbol.
    fn entity(&self, named: JobName) -> Entity<'_> {
        match named {
            JobName::Object(object) => Entity::Object(self.package, object),
            JobName::Method(receiver, name) => Entity::Method { receiver, name },
            // The job was made from the methods that are kept here.
            JobName::Operator(receiver, which) => Entity::Operator {
                receiver,
                method: &self.operator_decls[&receiver][which as usize],
            },
            JobName::Init(number) => Entity::InitFunc(self.package, number),
            JobName::Instance(package, object, at) => {
                let package = self.packages[package as usize];
                Entity::FuncInstance(package, object, &self.instances[at].args)
            }
            JobName::OwnMethod(receiver, name, at) => {
                let instance = &self.instances[at];
                Entity::MethodInstance {
                    receiver,
                    name,
                    args: &instance.args[instance.of_receiver..],
                }
            }
            JobName::Initializer(_) => Entity::PkgInit(self.package),
        }
    }

    /// The node that the function literals of the function of a job
    /// are numbered in: the function; for a first value its variable,
    /// or the function that gives it if no variable has a name.
    fn home_node(&self, namer: &mut Namer<'_>, job: &Job) -> Option<gopp_mangle::NodeId> {
        let named = self.initializer(job).and_then(|first| {
            let named = |object: &&ObjId| self.package.object(**object).name != self.blank;
            first.lhs.iter().find(named)
        });
        match named {
            Some(&object) => namer.entity(Entity::Object(self.package, object)).ok(),
            None => self.job_node(namer, job),
        }
    }

    /// The node of the function of a job, for its symbol: a function
    /// that gives variables their first value is the function with its
    /// number, from 1, of what starts the package.
    fn job_node(&self, namer: &mut Namer<'_>, job: &Job) -> Option<gopp_mangle::NodeId> {
        match job.named {
            JobName::Initializer(step) => {
                let start = namer.entity(Entity::PkgInit(self.package)).ok()?;
                namer.closure(start, step as u64 + 1).ok()
            }
            named => namer.entity(self.entity(named)).ok(),
        }
    }

    /// The step of the start of the program that the job makes, if it
    /// makes one.
    pub fn initializer(&self, job: &Job) -> Option<&gopp_sema::Initializer> {
        match job.named {
            JobName::Initializer(step) => self.package.init_order.get(step),
            _ => None,
        }
    }

    /// The symbols of the functions that give the variables of the
    /// package their first values, in the order of the start.
    pub fn initializers(&self) -> Vec<&str> {
        let mut steps = Vec::new();
        for job in &self.jobs {
            if let JobName::Initializer(step) = job.named {
                steps.push((step, &*job.symbol));
            }
        }
        steps.sort_unstable();
        steps.into_iter().map(|(_, symbol)| symbol).collect()
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

    /// Whether a value of the type starts with the address of a table
    /// of its own: it has a table, and its first parent has none whose
    /// address it could share (TYP-22).
    pub fn own_table(&self, ty: CheckedTypeId) -> bool {
        let first = self.types.parents(ty).first().copied();
        let shares = first.is_some_and(|first| self.types.has_table(first));
        self.types.has_table(ty) && !shares
    }

    /// The receiver of the method of the body, if the expression is the
    /// name `base` that stands for the parents of its type (TYP-20): no
    /// variable has the name there.
    pub fn base_of(&self, tree: &File, body: &Body, expr: ExprId) -> Option<gopp_sema::LocalId> {
        let named = matches!(tree.exprs[expr].kind, ExprKind::Name(name) if name == self.base);
        if !named || body.uses.contains_key(&expr) {
            return None;
        }
        let receiver = |local: &gopp_sema::Local| local.kind == LocalKind::Receiver;
        let at = body.locals.iter().position(receiver)?;
        Some(gopp_sema::LocalId(at as u32))
    }

    /// The `@Operator` method of the declared type `holder`, or of the
    /// instance of a generic type, that is the one at `which` among
    /// them.
    pub fn operator(&self, holder: CheckedTypeId, which: u32) -> Option<&Method> {
        match &*self.types.get(holder) {
            CheckedType::Named { id, args } if args.is_empty() => self.operators.get(&(*id, which)),
            CheckedType::Named { .. } => self.instance_operators.get(&(holder, which)),
            _ => None,
        }
    }

    /// What the checker recorded for the body of the function of a job.
    /// The `range` statements, the statements with labels and the jumps
    /// to labels of the declaration of the job, in the order of the
    /// source.
    pub fn jumps(&self, job: &Job) -> &[StmtId] {
        let known = self.jumps.get(&(job.file, job.at));
        known.map_or(&[], Vec::as_slice)
    }

    pub fn body(&self, job: &Job) -> &Body {
        match job.made {
            Some(at) => &self.instances[at].body,
            None => &self.info(job.file).bodies[job.at],
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
            let home = self.home(file);
            let id = *home.scope.get(&name.name)?;
            let object = home.object(id);
            let function = matches!(object.kind, ObjKind::Func) && object.span == name.span;
            return function.then_some((GenericOf::Func(self.package_of(file), id), made(None)));
        };
        let body = &self.info(file).bodies[at];
        let (named, pointer) = self.receiver(self.home(file), tree, body, receiver)?;
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
        file: u32,
        body: &Body,
        expr: ExprId,
    ) -> Option<(Used, Option<ExprId>)> {
        let (tree, _) = self.files[file as usize];
        let mut named = Self::unparen(tree, expr);
        if let ExprKind::Index { x, .. } = tree.exprs[named].kind {
            named = Self::unparen(tree, x);
        }
        match (&tree.exprs[named].kind, body.uses.get(&named)) {
            (ExprKind::Name(_), Some(Ref::Object(object))) => {
                Some((Used::Func(self.package_of(file), *object), None))
            }
            // A function of a package that the file imports.
            (ExprKind::Selector { .. }, Some(Ref::Member { import, member })) => {
                let ObjKind::Import { package, .. } = &self.home(file).object(*import).kind else {
                    return None;
                };
                let of = self
                    .packages
                    .iter()
                    .position(|known| std::ptr::eq(*known, &**package))?;
                Some((Used::Func(of as u32, *member), None))
            }
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
            JobName::Instance(package, object, at) => {
                let args = &self.instances[at].args;
                let package = self.packages[package as usize];
                Some(Entity::InstanceValue(package, object, args))
            }
            JobName::OwnMethod(receiver, name, at) => {
                let instance = &self.instances[at];
                Some(Entity::MethodInstanceValue {
                    receiver,
                    name,
                    args: &instance.args[instance.of_receiver..],
                })
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
    /// made side by side; then they are the next level. The instances
    /// are bounded (GEN-5): past a bound is one report, and no
    /// instance is made after it.
    fn instantiate(
        &mut self,
        generics: &FxHashMap<(u32, ObjId), Generic>,
        methods: &FxHashMap<NamedId, Vec<Generic>>,
        own: &FxHashMap<(NamedId, Spur), Generic>,
    ) {
        if generics.is_empty() && methods.is_empty() && own.is_empty() {
            return;
        }
        // The bounds, by the nodes of syntax of the package.
        let all = generics.values().chain(methods.values().flatten());
        let mut counted: Vec<bool> = (0..self.files.len())
            .map(|file| file < self.own_files)
            .collect();
        for generic in all.chain(own.values()) {
            counted[generic.file as usize] = true;
        }
        let code_of = self.decl_nodes(&counted);
        let nodes: u64 = (self.files[..self.own_files].iter())
            .map(|(tree, _)| (tree.exprs.len() + tree.stmts.len()) as u64)
            .sum();
        let limits = self.limits;
        let most_instances = limits
            .instances
            .0
            .max(limits.instances.1.saturating_mul(nodes));
        let most_code = limits.code.0.max(limits.code.1.saturating_mul(nodes));
        let nodes_of = |of: (u32, usize)| code_of.get(&of).copied().unwrap_or(0);
        let mut code: u64 = self
            .jobs
            .iter()
            .map(|job| nodes_of((job.file, job.at)))
            .sum();
        let mut instances = 0u64;
        let mut sizes: FxHashMap<CheckedTypeId, (u32, u64)> = FxHashMap::default();
        let all = generics.values().chain(methods.values().flatten());
        let written = self.written_in(all.chain(own.values()));
        self.found_steps += written.len() as u64;

        let mut by_symbol: FxHashMap<String, usize> = FxHashMap::default();
        let mut seen: FxHashSet<CheckedTypeId> = FxHashSet::default();
        let mut level = 0..self.jobs.len();
        while !level.is_empty() {
            let mut found: Vec<Found> = Vec::new();
            let mut known: FxHashMap<(Used, Vec<CheckedTypeId>), usize> = FxHashMap::default();
            let mut of_types: Vec<((CheckedTypeId, Spur), usize, bool)> = Vec::new();
            let mut of_operators = Vec::new();
            let mut steps = 0;
            // The first instance that is refused: the report, and the
            // use that asks for the instance.
            let mut refused: Option<(Code, String, (usize, Span))> = None;
            let bounded = |used: String, bound: String| {
                let message = format!("cannot instantiate {used}: {bound} (GEN-5)");
                (Code::InstTooLarge, message)
            };
            // A type argument may be a type that a function declares.
            let locals = |named: NamedId| self.local(named);
            let mut namer = Namer::new(self.types, self.names).with_locals(&locals);
            let mut types: Vec<(String, CheckedTypeId, usize)> = Vec::new();
            // One more instance, of so many nodes of syntax: the bound
            // that it passes, if it passes one.
            let mut one_more = |of: Generic| {
                instances += 1;
                code = code.saturating_add(nodes_of((of.file, of.at)));
                if instances > most_instances {
                    return Some(format!(
                        "the package needs more than {most_instances} instances"
                    ));
                }
                (code > most_code).then(|| {
                    format!("the instances of the package hold more than {most_code} nodes of code")
                })
            };
            'level: for at in level {
                let job = &self.jobs[at];
                let (tree, _) = self.files[job.file as usize];
                let body = self.body(job);
                // In an instance: the body as it is written, which says
                // what is built around a type parameter.
                let generic = (job.made).map(|_| &self.info(job.file).bodies[job.at]);
                let mut uses: Vec<_> = (body.instances.iter())
                    .filter_map(|(&expr, instance)| {
                        let (used, _) = self.generic_named(job.file, body, expr)?;
                        let span = tree.exprs[expr].span;
                        Some((span.lo, span.hi, used, instance, expr))
                    })
                    .collect();
                uses.sort_unstable_by_key(|&(lo, hi, ..)| (lo, hi));
                steps += uses.len() as u64;
                for (_, _, used, instance, expr) in uses {
                    let key = (used, instance.args.clone());
                    if self.instance_jobs.contains_key(&key) || known.contains_key(&key) {
                        continue;
                    }
                    let by = (at, tree.exprs[expr].span);
                    // What is used, with the type arguments of its
                    // receiver before its own.
                    let (of, named, entity, of_receiver) = match used {
                        Used::Func(package, object) => {
                            let of = self.packages[package as usize];
                            let entity = Entity::FuncInstance(of, object, &instance.args);
                            (
                                generics.get(&(package, object)),
                                Named::Func(package, object),
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
                    let Some(&of) = of else {
                        continue;
                    };
                    // A type argument that the instance builds around a
                    // type parameter is bounded.
                    let as_written = generic.and_then(|generic| generic.instances.get(&expr));
                    let built = as_written.into_iter().flat_map(|written| {
                        let both = written.args.iter().zip(&instance.args);
                        both.filter(|&(&from, _)| self.built_around(from))
                    });
                    let mut bound = None;
                    for (_, &arg) in built {
                        bound = bound.or_else(|| self.too_large(arg, &mut sizes, &mut steps));
                    }
                    let Ok(symbol) = symbol_of(&mut namer, entity) else {
                        continue;
                    };
                    let job = match (by_symbol.get(&symbol), bound) {
                        (Some(&job), None) => job,
                        (_, bound) => {
                            if let Some(bound) = bound.or_else(|| one_more(of)) {
                                let (code, message) = bounded(self.used_name(used), bound);
                                refused = Some((code, message, by));
                                break 'level;
                            }
                            let mut args = of_receiver;
                            let of_receiver = args.len();
                            args.extend(&instance.args);
                            let job = self.jobs.len() + found.len();
                            by_symbol.insert(symbol.clone(), job);
                            found.push(Found {
                                of,
                                by,
                                named,
                                args,
                                of_receiver,
                                sig: instance.ty,
                                symbol,
                            });
                            job
                        }
                    };
                    known.insert(key, job);
                }
                // The instances of generic types with methods that the
                // types of the body are written with.
                let typed = body.types.values().map(|typed| typed.ty);
                let mut rest: Vec<CheckedTypeId> = typed
                    .chain(body.locals.iter().map(|local| local.ty))
                    .collect();
                // The types of a body are met in no order: of those that
                // are refused, the one whose report comes first is.
                let mut unfit: Vec<(String, Code, (usize, Span))> = Vec::new();
                while let Some(ty) = rest.pop() {
                    if !seen.insert(ty) {
                        continue;
                    }
                    steps += 1;
                    let kind = self.types.get(ty);
                    kind.each_part(&mut |part| rest.push(part));
                    // What a type argument makes of the types of an
                    // instance is held to what the types ask (OWN-P9,
                    // TYP-15), at the use that asks for the instance.
                    if let (Some(by), Some((code, message))) = (job.by, self.unfit(job, ty)) {
                        unfit.push((message, code, by));
                    }
                    let CheckedType::Named { id, args } = &*kind else {
                        continue;
                    };
                    rest.push(self.types.underlying(ty));
                    // A parent may be an instance of a generic type.
                    rest.extend(self.types.parents(ty));
                    if args.is_empty() {
                        continue;
                    }
                    // An instance that the source writes nowhere was
                    // built around a type parameter.
                    if job.made.is_some() && !written.contains(&ty) {
                        let mut bound = None;
                        for &arg in args.iter() {
                            bound = bound.or_else(|| self.too_large(arg, &mut sizes, &mut steps));
                        }
                        if let Some(bound) = bound {
                            let name = self.names.resolve(&self.types.home(*id).name);
                            let (code, message) = bounded(name.to_owned(), bound);
                            unfit.push((message, code, (at, self.name_span(job))));
                        }
                    }
                    if methods.contains_key(id)
                        && let Ok(symbol) = symbol_of(&mut namer, Entity::TypeInfo(ty))
                    {
                        types.push((symbol, ty, at));
                    }
                }
                let first = unfit.into_iter().min_by(|a, b| a.0.cmp(&b.0));
                if let Some((message, code, by)) = first {
                    refused = Some((code, message, by));
                    break 'level;
                }
            }
            types.sort_unstable();
            for (_, ty, at) in types {
                if refused.is_some() {
                    break;
                }
                let CheckedType::Named { id, args } = &*self.types.get(ty) else {
                    continue;
                };
                for &of in methods.get(id).into_iter().flatten() {
                    let Some((name, pointer)) = of.method else {
                        continue;
                    };
                    // An `@Operator` method is the one at its place
                    // among those of the type: two may have one name.
                    let (tree, _) = self.files[of.file as usize];
                    let which = match &tree.decl_nodes[of.decl].kind {
                        DeclKind::Func { name, .. } => {
                            let operators = &self.home(of.file).operators;
                            operators.get(&name.span.lo).map(|&(_, which)| which)
                        }
                        _ => None,
                    };
                    let declared = match which {
                        Some(_) => self.types.declared_operators(*id),
                        None => Vec::new(),
                    };
                    let (sig, entity) = match which.map(|which| &declared[which as usize]) {
                        Some(method) => {
                            let mut own = self.types.operators(ty, name).into_iter();
                            let own = own.find(|own| own.holder == ty && Some(own.at) == which);
                            let Some(own) = own else {
                                continue;
                            };
                            let entity = Entity::Operator {
                                receiver: ty,
                                method,
                            };
                            (own.sig, entity)
                        }
                        None => {
                            let path = self.path_of(of.file);
                            let found = self.types.lookup(ty, name, path, self.names, false);
                            let sig = match found.map(|found| found.member) {
                                Ok(gopp_sema::types::Member::Method { sig, .. }) => sig,
                                _ => continue,
                            };
                            (sig, Entity::Method { receiver: ty, name })
                        }
                    };
                    let Ok(symbol) = symbol_of(&mut namer, entity) else {
                        continue;
                    };
                    let named = match which {
                        Some(which) => Named::Operator(ty, which),
                        None => Named::Method(ty, name),
                    };
                    steps += 1;
                    let job = match by_symbol.get(&symbol) {
                        Some(&job) => job,
                        None => {
                            let by = (at, self.name_span(&self.jobs[at]));
                            if let Some(bound) = one_more(of) {
                                let used = self.used_name(Used::Method(ty, name));
                                let (code, message) = bounded(used, bound);
                                refused = Some((code, message, by));
                                break;
                            }
                            let job = self.jobs.len() + found.len();
                            by_symbol.insert(symbol.clone(), job);
                            found.push(Found {
                                of,
                                by,
                                named,
                                args: args.to_vec(),
                                of_receiver: args.len(),
                                sig,
                                symbol,
                            });
                            job
                        }
                    };
                    // The one `@Operator` method of its name is called
                    // by the name like any other method.
                    let alone = declared.iter().filter(|other| other.method.name == name);
                    if which.is_none() || alone.count() == 1 {
                        of_types.push(((ty, name), job, pointer));
                    }
                    if let Some(which) = which {
                        of_operators.push(((ty, which), job, declared));
                    }
                }
            }
            self.found_steps += steps;
            if let Some((code, message, (by, at))) = refused {
                let report = Diagnostic::new(code, at, message);
                self.refused.push(self.led(by, report));
                return;
            }
            self.instance_jobs.extend(known);
            let bodies: Vec<Body> = (found.par_iter())
                .map(|found| {
                    let Generic { file, at, .. } = found.of;
                    let generic = &self.info(file).bodies[at];
                    let written = gopp_sema::Written {
                        types: self.types,
                        names: self.names,
                        package: self.path_of(file),
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
                    Named::Func(package, object) => {
                        (JobName::Instance(package, object, made), None)
                    }
                    Named::Method(ty, name) => (JobName::Method(ty, name), Some(receiver(ty))),
                    Named::Operator(ty, which) => {
                        (JobName::Operator(ty, which), Some(receiver(ty)))
                    }
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
                    by: Some(found.by),
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
                let method = Method {
                    symbol: job.symbol.clone(),
                    receiver,
                    sig: job.sig,
                };
                self.instance_methods.insert(key, method);
            }
            for (key, job, declared) in of_operators {
                self.operator_decls.entry(key.0).or_insert(declared);
                let job = &self.jobs[job];
                let Some(receiver) = job.receiver else {
                    continue;
                };
                let method = Method {
                    symbol: job.symbol.clone(),
                    receiver,
                    sig: job.sig,
                };
                self.instance_operators.insert(key, method);
            }
            level = start..self.jobs.len();
        }
    }

    /// How many nodes of syntax each declaration has, for the files
    /// that are `counted`: those of the package, and those with generic
    /// code.
    fn decl_nodes(&self, counted: &[bool]) -> FxHashMap<(u32, usize), u64> {
        let mut nodes = FxHashMap::default();
        for (file, (tree, _)) in self.files.iter().enumerate() {
            if !counted[file] {
                continue;
            }
            let mut decls: Vec<(u32, u32, usize)> = (tree.decls.iter().enumerate())
                .map(|(at, &decl)| {
                    let span = tree.decl_nodes[decl].span;
                    (span.lo, span.hi, at)
                })
                .collect();
            decls.sort_unstable();
            let exprs = tree.exprs.iter().map(|(_, expr)| expr.span);
            for span in exprs.chain(tree.stmts.iter().map(|(_, stmt)| stmt.span)) {
                let before = decls.partition_point(|&(start, _, _)| start <= span.lo);
                let around = before.checked_sub(1).map(|at| decls[at]);
                if let Some((_, _, at)) = around.filter(|&(_, end, _)| span.hi <= end) {
                    *nodes.entry((file as u32, at)).or_default() += 1;
                }
            }
        }
        nodes
    }

    /// The types without a type parameter that the generic bodies are
    /// written with, and their parts: the source writes them.
    fn written_in<'g>(
        &self,
        generic: impl Iterator<Item = &'g Generic>,
    ) -> FxHashSet<CheckedTypeId> {
        let mut written = FxHashSet::default();
        let mut walked = FxHashSet::default();
        for of in generic {
            let body = &self.info(of.file).bodies[of.at];
            let typed = body.types.values().map(|typed| typed.ty);
            let mut rest: Vec<CheckedTypeId> = typed
                .chain(body.locals.iter().map(|local| local.ty))
                .collect();
            while let Some(ty) = rest.pop() {
                if !walked.insert(ty) {
                    continue;
                }
                if !self.types.has_param(ty) {
                    written.insert(ty);
                }
                self.types.get(ty).each_part(&mut |part| rest.push(part));
            }
        }
        written
    }

    /// Whether the type is built around a type parameter: written with
    /// one, and not one itself, which is passed on as it is.
    fn built_around(&self, ty: CheckedTypeId) -> bool {
        self.types.has_param(ty) && !matches!(&*self.types.get(ty), CheckedType::Param(_))
    }

    /// The bound that a type argument which generic code built passes,
    /// if it passes one: how deep it nests, and how many parts it has
    /// as a tree. `sizes` has both for the types that were measured.
    fn too_large(
        &self,
        ty: CheckedTypeId,
        sizes: &mut FxHashMap<CheckedTypeId, (u32, u64)>,
        steps: &mut u64,
    ) -> Option<String> {
        let mut rest = vec![(ty, false)];
        while let Some((ty, parts_done)) = rest.pop() {
            if sizes.contains_key(&ty) {
                continue;
            }
            *steps += 1;
            let mut parts = Vec::new();
            self.types.get(ty).each_part(&mut |part| parts.push(part));
            if parts_done {
                let of_parts = parts
                    .iter()
                    .map(|part| sizes.get(part).copied().unwrap_or((0, 1)));
                let size = of_parts.fold((0u32, 1u64), |(height, count), (deep, many)| {
                    (height.max(deep + 1), count.saturating_add(many))
                });
                sizes.insert(ty, size);
            } else {
                rest.push((ty, true));
                rest.extend(parts.into_iter().map(|part| (part, false)));
            }
        }
        let (height, parts) = sizes.get(&ty).copied().unwrap_or((0, 1));
        let limits = self.limits;
        if height > limits.height {
            return Some(format!(
                "a type argument nests more than {} levels",
                limits.height
            ));
        }
        (parts > limits.parts).then(|| {
            format!(
                "a type argument has more than {} parts as a tree",
                limits.parts
            )
        })
    }

    /// What a use names, as a report says it.
    fn used_name(&self, used: Used) -> String {
        match used {
            Used::Func(package, object) => {
                let name = self.packages[package as usize].object(object).name;
                self.names.resolve(&name).to_owned()
            }
            Used::Method(receiver, name) => {
                let of = match &*self.types.get(receiver) {
                    CheckedType::Named { id, .. } => self.types.home(*id).name,
                    _ => self.blank,
                };
                format!("{}.{}", self.names.resolve(&of), self.names.resolve(&name))
            }
        }
    }

    /// Where the name of the function of a job is declared.
    fn name_span(&self, job: &Job) -> Span {
        let (tree, _) = self.files[job.file as usize];
        match &tree.decl_nodes[job.decl].kind {
            DeclKind::Func { name, .. } => name.span,
            _ => tree.decl_nodes[job.decl].span,
        }
    }

    /// What the type `ty` of the body of the instance `job` is refused
    /// for: an option of a type that has nil already (`E0446`), or an
    /// instance of a generic type with a `@Required` field of a type
    /// parameter that is an option here (`E0363`).
    fn unfit(&self, job: &Job, ty: CheckedTypeId) -> Option<(Code, String)> {
        let made = job.made?;
        let types = self.types;
        match &*types.get(ty) {
            CheckedType::Option(of) if types.has_own_nil(*of, self.names) => {
                // The type parameter that the type argument is given for.
                let generic = &self.info(job.file).bodies[job.at];
                let args = self.instances[made].args.iter();
                let param = (args.zip(&generic.type_params))
                    .find(|&(arg, _)| arg == of)
                    .and_then(|(_, &param)| types.param_name(param));
                let of = self.shown(*of);
                let whose = match param {
                    Some(param) => format!(" for {}", self.names.resolve(&param)),
                    None => String::new(),
                };
                let message = format!(
                    "cannot use {of}{whose}: an Option of it is written, and {of} has nil already"
                );
                Some((Code::InstOption, message))
            }
            CheckedType::Named { id, args } => {
                let params = types.type_params(*id);
                let option = (params.iter().zip(args.iter()))
                    .find(|&(&param, &arg)| types.is_required(param) && types.is_option(arg));
                option.map(|(&param, &arg)| {
                    let message = format!(
                        "cannot use {} for {}: a field of it is @Required, and an option may \
                         be nil",
                        self.shown(arg),
                        self.shown(param)
                    );
                    (Code::OptionAndRequired, message)
                })
            }
            _ => None,
        }
    }

    /// The type as a report of the package writes it.
    pub fn shown(&self, ty: CheckedTypeId) -> String {
        match self.names.get(&self.package.name) {
            Some(package) => self.types.display_in(ty, self.names, package),
            None => self.types.display(ty, self.names),
        }
    }

    /// The report with the uses that led to the function of the job,
    /// if it is an instance: the nearest first.
    pub fn led(&self, mut job: usize, mut report: Diagnostic) -> Diagnostic {
        let mut led = 0;
        while let Some((before, at)) = self.jobs[job].by {
            led += 1;
            if led > MAX_LED {
                break;
            }
            let name = match self.jobs[job].named {
                JobName::Instance(package, object, _) => {
                    self.used_name(Used::Func(package, object))
                }
                JobName::Method(ty, name) | JobName::OwnMethod(ty, name, _) => {
                    self.used_name(Used::Method(ty, name))
                }
                JobName::Object(_)
                | JobName::Operator(..)
                | JobName::Init(_)
                | JobName::Initializer(_) => break,
            };
            let note = format!("{name} is instantiated here");
            report = report.with_note(Code::NoteRefersTo, Some(at), note);
            job = before;
        }
        report
    }

    /// The report with the uses that led to the instance `job`.
    pub fn led_to(&self, job: &Job, report: Diagnostic) -> Diagnostic {
        match self.jobs.iter().position(|other| std::ptr::eq(other, job)) {
            Some(at) => self.led(at, report),
            None => report,
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
            by: None,
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
        let (named, pointer) = self.receiver(self.home(file), tree, body, receiver)?;
        let kind = types.get(named);
        let CheckedType::Named { id, args } = &*kind else {
            return None;
        };
        if !args.is_empty() || !types.type_params(*id).is_empty() {
            return None;
        }
        let receiver = match pointer {
            true => types.pointer(named),
            false => named,
        };
        if let Some(&(_, which)) = package.operators.get(&name.span.lo) {
            let declared = types.declared_operators(*id);
            let sig = declared.get(which as usize)?.method.sig;
            return Some(job(JobName::Operator(named, which), Some(receiver), sig));
        }
        let found = types.lookup(named, name.name, self.path, names, false);
        let gopp_sema::types::Member::Method { sig, .. } = found.ok()?.member else {
            return None;
        };
        Some(job(JobName::Method(named, name.name), Some(receiver), sig))
    }

    /// The declared type that a receiver names, and whether the
    /// receiver is a pointer to it.
    fn receiver(
        &self,
        home: &Package,
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
            Ref::Object(object) => Some((home.object(*object).ty, pointer)),
            _ => None,
        }
    }

    /// The variables of the package and what it declares for C, in a
    /// module that is merged first.
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
                    defined: true,
                });
            }
        }
        // What starts the package gives the variables their first
        // values, one step after another.
        func::lower_start(self, &mut module, &mut memo);
        cabi::lower_c_side(self, &mut module, &mut memo);
        Part {
            source: merge::Source::new(module),
            diagnostics: memo.diagnostics,
            steps: memo.steps,
            glues: memo.glues,
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
            loop {
                if let Some((job, literal)) = memo.literals.pop() {
                    func::lower_literal(self, &mut module, &mut memo, job, literal);
                } else if let Some((job, stmt)) = memo.ranges.pop() {
                    func::lower_ranged(self, &mut module, &mut memo, job, stmt);
                } else if !func::lower_later(self, &mut module, &mut memo) {
                    break;
                }
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
                    Made::Adjusted { name, by } => {
                        func::lower_adjusted(self, &mut module, &mut memo, &symbol, (ty, name, by));
                    }
                    Made::Value(of) => {
                        func::lower_value(self, &mut module, &mut memo, &symbol, (&of, ty), false);
                    }
                    Made::ValueOfC(of) => {
                        func::lower_value(self, &mut module, &mut memo, &symbol, (&of, ty), true);
                    }
                    Made::CThunk(of) => {
                        cabi::lower_c_thunk(self, &mut module, &mut memo, &symbol, (&of, ty));
                    }
                    Made::MethodValue(..) | Made::MethodExpr { .. } => {
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
