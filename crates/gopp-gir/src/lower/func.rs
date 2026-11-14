//! One function: its parameters, its variables and the scopes they
//! live in.

use gopp_diag::{Code, Diagnostic};
use gopp_sema::types::TypeId as CheckedTypeId;
use gopp_sema::{Body, LocalId as CheckedLocalId};
use gopp_span::{SourceFile, Span as SourceSpan};
use gopp_syntax::ast::{
    Block, BranchKind, DeclKind, ExprId, ExprKind, File, Param, StmtId, StmtKind,
};
use lasso::Spur;
use rustc_hash::{FxHashMap, FxHashSet};

use super::facts::Facts;
use super::glue::{Ends, Glue};
use super::{Callee, Job, Memo, Shared};
use crate::{
    BlockId, BuildError, Constant, ConstantKind, FuncBuilder, FuncId, Linkage, LocalId, Module,
    Operand, Place, ProjectionKind, Rvalue, ScopeKind, Span, Type, TypeId, TypeInterner,
    UnwindAction,
};

pub(super) type Built<T = ()> = Result<T, BuildError>;

/// In the place of what is not supported: nothing more is reported,
/// because the declaration of what is used says what is wrong with it.
pub(super) const SAID: &str = "";

/// The stack that is left when a walk goes one level deeper, and how
/// much is added when it is not.
const STACK_LEFT: usize = 128 * 1024;
const STACK_MORE: usize = 2 * 1024 * 1024;

/// Runs `walk` with enough stack for one more level of the tree.
pub(super) fn deeper<T>(walk: impl FnOnce() -> T) -> T {
    stacker::maybe_grow(STACK_LEFT, STACK_MORE, walk)
}

/// The variables that a block of the source declares, in their order.
#[derive(Default)]
pub(super) struct Scope {
    vars: Vec<LocalId>,
    /// The places of the block that own what is freed, each with the
    /// drop glue of its type: variables, parameters, and what a loop
    /// walks. They end where the block is left, the last first.
    owned: Vec<(Place, Ends)>,
    /// The walks of the range loops over maps that the block has: what
    /// the runtime keeps of each, by its address.
    walks: Vec<Operand>,
    /// The variables of the block that live in cells, because a
    /// function literal uses them.
    cells: Vec<CheckedLocalId>,
    /// Whether the block has a debug scope: it gets one with its first
    /// variable.
    debug: bool,
    unwind: Chain,
}

/// The blocks that unwinding runs for what a scope owns (PAN-6). Each
/// ends one place and goes on with the block of the place before it, so
/// the blocks are as many as the places, however many calls there are.
#[derive(Default)]
struct Chain {
    /// What the blocks stand on: the first block of the scope around
    /// this one. Not known before the first block is asked for.
    base: Option<Option<BlockId>>,
    /// The first block to run for the owned places up to each one.
    owned: Vec<Option<BlockId>>,
    /// The same for the walks up to each one, which stand on the owned
    /// places, and how many of those there were then.
    walks: Vec<Option<BlockId>>,
    walks_on: usize,
    /// Every place of the scope has its block.
    whole: bool,
}

/// A `defer` statement of the function (PAN-3).
pub(super) struct Deferred {
    pub stmt: StmtId,
    /// The statement may run more than once, in a loop or in a function
    /// with labels: which list of the function has its records then.
    pub list: Option<usize>,
    /// What the statement evaluated, once it is lowered.
    pub call: Option<Pending>,
}

/// What a `defer` statement calls.
#[derive(Clone)]
pub(super) enum Waits {
    /// A function or a method.
    Call(Callee),
    /// `print`, or `println` if the flag is set.
    Print(bool),
    /// A function value, which is the first of what the statement
    /// keeps: the type of its code.
    Value(TypeId),
    /// `panic(v)`, with `v` in an interface value.
    Panic,
    /// `delete(m, k)` and `clear(m)` for a map: the statement keeps the
    /// address of the place that holds the map, and the key.
    Delete(super::map::MapOf),
    Clear(super::map::MapOf),
    /// `recover()`, which stops no panic when it is the deferred call
    /// itself, as in Go.
    Nothing,
}

/// A call that waits for its function to be left.
#[derive(Clone)]
pub(super) struct Pending {
    pub what: Waits,
    /// The record of the statement, a slot of its function, and its
    /// type: whether the statement ran, then the receiver and the
    /// arguments, which the record owns. A statement of a list has no
    /// slot: each time it runs it makes a record in a cell, which
    /// starts with the record that was the first of the list before and
    /// the number of the statement.
    pub record: Option<LocalId>,
    pub ty: TypeId,
    /// How each of them ends if the call does not take it.
    pub slots: Vec<Option<Ends>>,
    /// How what the call gives ends: nothing takes it. Each with the
    /// number of the result that ends so, where the call gives several.
    pub result: Vec<(Option<u32>, Ends)>,
    /// The call has `!`.
    pub marked: Option<Marked>,
    pub span: SourceSpan,
}

/// What a deferred call with `!` does with an error (ERR-11a).
#[derive(Clone)]
pub(super) struct Marked {
    /// The function can fail: the error becomes its own if it has none
    /// and no panic runs the call. The last that the record keeps is
    /// the address of the result with the error then, which is no
    /// argument of the call. In any other function the error is a
    /// panic.
    pub sets: bool,
    /// The call as it is written, and where: what a panic names.
    pub shown: String,
    pub place: String,
    /// How the error ends where nothing takes it.
    pub ends: Option<Ends>,
}

/// What the runtime knows a function with `defer` statements by, from
/// its start to its end (PAN-1).
#[derive(Clone, Copy)]
struct Frame {
    /// `gopp_frame` of the runtime; none where a panic ends the
    /// program at once, and no deferred call is made for one.
    node: Option<LocalId>,
    /// The addresses of the records of the statements, in the order of
    /// the source, and then those of the results: a slot whose address
    /// is known outside has its value in memory at every call, where a
    /// panic may leave the function.
    records: LocalId,
    /// The function that makes the deferred calls.
    run: FuncId,
}

/// The deferred calls of a function, for the function that makes them.
pub(super) struct Outlined {
    symbol: String,
    /// A loop over a function has `defer` statements in its body: what
    /// they call is known when the body is lowered.
    late: bool,
    /// How many addresses the function has for it.
    count: usize,
    /// The list of each statement, and its call if it has one.
    calls: Vec<(Option<usize>, Option<Pending>)>,
}

/// A label, known when the list of statements that has it is entered.
pub(super) struct Label {
    pub block: BlockId,
    /// How many scopes are open where it is.
    depth: usize,
    /// How many variables its scope has where it is, and how many
    /// places that own, once it is met.
    live: Option<(usize, usize)>,
}

/// A statement that `break` or `continue` leaves.
pub(super) struct Target {
    pub label: Option<Spur>,
    pub breaks: BlockId,
    /// Where `continue` goes; none for a `switch`.
    pub continues: Option<BlockId>,
    /// How many scopes are open around its body.
    pub depth: usize,
    /// For a clause of a `switch`: the body of the next clause.
    pub next: Option<BlockId>,
}

pub(super) struct FuncCx<'a, 'm> {
    pub shared: &'a Shared<'a>,
    pub memo: &'m mut Memo<'a>,
    pub b: FuncBuilder<'m>,
    pub tree: &'a File,
    pub source: &'a SourceFile,
    /// Which of the files that the lowering has: one of the package, or
    /// of a package that it imports for an instance of its generic code.
    pub(super) file: u32,
    pub body: &'a Body,
    /// What the function does to its parameters and its variables.
    facts: &'a Facts,
    /// The arguments of the call that is lowered which are lent: each
    /// is read where it is, and no copy of it is made.
    pub lent: FxHashSet<ExprId>,
    /// The places that the checker marks as moved which give a copy:
    /// those of an exchange of three places or more of which two may
    /// be one.
    pub copied: FxHashSet<ExprId>,
    /// The parameters that the function does not own: its caller owns
    /// their values.
    lent_params: FxHashSet<LocalId>,
    /// Those of them that the function calls methods through: the
    /// caller lent a value that nothing else reads.
    worked_params: FxHashSet<LocalId>,
    /// The arguments of the call that is lowered which an interface
    /// parameter is lent without a box, each with whether the function
    /// calls methods through the parameter.
    pub unboxed: FxHashMap<ExprId, bool>,
    /// The receivers of the calls that are lowered which are a part of
    /// the value of their expression, each with the type of the part.
    pub parts: FxHashMap<ExprId, CheckedTypeId>,
    /// For each variable that lives in a cell and is known here: the
    /// slot with the address of the cell. The variable itself is
    /// another name of what the cell holds.
    pub cells: FxHashMap<CheckedLocalId, LocalId>,
    /// The variables that the function literal that is lowered uses of
    /// the functions around it, in the order of its environment.
    pub captured: Vec<CheckedLocalId>,
    /// The environment of that literal is in a frame: it has the
    /// addresses of the variables, or of their cells.
    pub in_frame: bool,
    /// What the body of a loop over a function, which is lowered as a
    /// function of its own, has of the function with the loop.
    pub ranged: Option<super::ranged::Leaving>,
    /// The body of a loop over a function has `defer` statements of
    /// this function.
    pub late_defers: bool,
    /// The function literal that a `defer` statement in such a body
    /// calls, while its value is worked out.
    pub deferring: Option<ExprId>,
    /// The local of each variable that is another name of what a cell
    /// holds.
    cell_vars: FxHashMap<LocalId, CheckedLocalId>,
    vars: FxHashMap<CheckedLocalId, LocalId>,
    scopes: Vec<Scope>,
    pub labels: FxHashMap<Spur, Label>,
    pub targets: Vec<Target>,
    /// The variable of each result that has a name.
    pub results: Vec<Option<LocalId>>,
    /// The label of the statement that is lowered next.
    pub label: Option<Spur>,
    /// Whether evaluating an expression can change what another reads.
    effects: FxHashMap<ExprId, bool>,
    reported: FxHashSet<(u32, u32)>,
    /// The place on the right of `s = append(s, ...)` and of
    /// `s = s[i:j]`: its slice is used where it is, and not copied.
    pub reused: Option<ExprId>,
    /// The variables `&v` of range clauses: each holds the address of
    /// the element that it names.
    pub aliases: FxHashSet<LocalId>,
    /// For each result, the place of the caller for the record of a
    /// view, if the result points to a slice.
    pub views: Vec<Option<LocalId>>,
    /// The places of the caller that go to what is written directly in
    /// the `return` that is lowered: a view, which is built there, or a
    /// call, which passes them on (SLC-8).
    pub outer: FxHashMap<ExprId, Vec<LocalId>>,
    /// The temporaries that own a value which nothing has taken yet,
    /// each with the drop glue of its type: what is left of them when
    /// their statement ends is freed. They are listed in the order in
    /// which they were made; `owning` says which of them still own.
    pub temps: Vec<(Place, Ends)>,
    owning: FxHashMap<(LocalId, u32), usize>,
    /// The variables that do not own their values: a plain range
    /// variable is the element as it is.
    borrowed: FxHashSet<LocalId>,
    /// The names in the `return` that is lowered whose variables end
    /// with the function: each hands its value over.
    pub handed: FxHashSet<ExprId>,
    /// The `defer` statements of the function, in the order of the
    /// source.
    pub defers: Vec<Deferred>,
    /// The function has a label or a `goto`: a statement may run more
    /// than once outside every loop.
    pub jumps: bool,
    /// The function is declared with `!`: its last result is the error
    /// that the mark adds, which no result of the source is (ERR-1).
    pub fails: bool,
    /// The function of the package that is lowered, and the function
    /// literal in it if it is one that is lowered; none for a function
    /// that the compiler makes.
    pub owner: Option<(&'a Job, Option<ExprId>)>,
    /// For a function with `defer` statements.
    frame: Option<Frame>,
    /// The symbol of the function that makes its deferred calls.
    deferred: Option<String>,
    /// For each list of records of `defer` statements that run more
    /// than once: the slot with the first record, the one made last.
    lists: Vec<LocalId>,
    /// The variables of blocks that a `defer` statement may borrow and
    /// that this function declares: the slot with the cell of each,
    /// the type of the cell with what the list of borrowed variables
    /// keeps after the variable, and what ends the variable.
    extendable: FxHashMap<CheckedLocalId, (LocalId, TypeId, Option<FuncId>)>,
    /// How many such variables were declared, while the program runs:
    /// each has its number, and the one with the highest ends first.
    declared: Option<LocalId>,
    /// The slots with the first borrowed variable of each list, and
    /// the list of the block of the function that is lowered now.
    kept_lists: Vec<LocalId>,
    region: Option<LocalId>,
    /// How often what the scopes and the temporaries own has changed.
    owners: u64,
    /// The first block that unwinding runs for what is owned, as of
    /// that many changes.
    unwind_head: Option<(u64, Option<BlockId>)>,
    /// The first block to run for the temporaries up to each one, and
    /// the block of the innermost scope that they stand on.
    temp_heads: Vec<Option<BlockId>>,
    temp_base: Option<BlockId>,
    /// Not zero while the end of a value is written: a panic there
    /// ends the program (PAN-5).
    pub ending: u32,
    /// What is written is no place where a panic leaves the function.
    quiet: bool,
    /// For a function with a frame: the block that asks the runtime
    /// whether the function goes on, and ends the results if not.
    frame_block: Option<BlockId>,
    /// Which of the owned places of the outermost scope are the
    /// results, and the results without a name that own a value.
    results_owned: std::ops::Range<usize>,
    hidden: Vec<(LocalId, Ends)>,
    /// The places whose values were moved out and that are still to be
    /// made empty: what is evaluated beside the move reads them as they
    /// were (`return s, len(s)`).
    pub moved: Vec<(Place, TypeId)>,
}

/// Adds the function of the job to the module, or reports why not.
pub(super) fn lower_function<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    job: &'a Job,
) {
    if let Some(first) = shared.initializer(job) {
        return lower_initializer(shared, module, memo, job, first);
    }
    let (tree, source) = shared.files[job.file as usize];
    let DeclKind::Func {
        receiver,
        name,
        ty,
        body: Some(block),
        ..
    } = &tree.decl_nodes[job.decl].kind
    else {
        return;
    };
    if job.symbol.is_empty() {
        let message = "the function has no symbol".to_owned();
        let diagnostic = Diagnostic::new(Code::GirVerify, name.span, message);
        memo.diagnostics.push(diagnostic);
        return;
    }
    let sig = match memo.signature(shared, &mut module.types, job.receiver, job.sig) {
        Ok(sig) => sig,
        Err(what) => {
            let message = format!("not supported yet: {what}");
            let diagnostic = Diagnostic::new(Code::GirUnsupported, name.span, message);
            memo.diagnostics.push(diagnostic);
            return;
        }
    };
    let at = source.line_col(name.span.lo);
    let loc = Span {
        file: job.file + 1,
        line: at.line,
        col: at.column,
    };
    // Every package that uses an instance has it; the linker keeps one.
    let link = match shared.is_instance(job) {
        true => Linkage::LinkOnceOdr,
        false => Linkage::External,
    };
    let built = FuncBuilder::new(module, &job.symbol, sig, link, loc).and_then(|b| {
        let body = shared.body(job);
        let mut cx = FuncCx::new(shared, &mut *memo, b, job.file, (body, &job.facts));
        cx.owner = Some((job, None));
        // `@Inline` and `@NoInline`, of which a declaration has one.
        let own = tree.decl_nodes[job.decl].attrs.iter();
        for attr in own.filter(|attr| attr.path.package.is_none()) {
            match shared.names.resolve(&attr.path.name.name) {
                "Inline" => cx.b.set_inline(crate::Inline::Always),
                "NoInline" => cx.b.set_inline(crate::Inline::Never),
                _ => {}
            }
        }
        cx.function(receiver.as_ref(), *ty, block, 0)?;
        cx.b.drop_unreached();
        let outlined = cx.outlined();
        cx.b.finish()?;
        Ok(outlined)
    });
    let built = built.and_then(|outlined| match outlined {
        Some(outlined) if outlined.late => {
            memo.later.push(Later(job, None, loc, outlined));
            Ok(())
        }
        Some(outlined) => lower_deferred(shared, module, memo, job, loc, outlined),
        None => Ok(()),
    });
    if let Err(error) = built {
        let message = format!("{} could not be lowered: {error}", job.symbol);
        let diagnostic = Diagnostic::new(Code::GirVerify, name.span, message);
        memo.diagnostics.push(diagnostic);
    }
}

/// Adds what starts the package to the module, if a variable of the
/// package has a first value: the function that calls, in the order of
/// the start, each function that gives variables their first value.
pub(super) fn lower_start<'a>(shared: &'a Shared<'a>, module: &mut Module, memo: &mut Memo<'a>) {
    let steps = shared.initializers();
    if steps.is_empty() {
        return;
    }
    let symbol = gopp_mangle::pkg_init_symbol(&shared.package.path);
    let sig = module.types.intern(Type::Func(crate::FuncType {
        params: [].into(),
        ret: TypeInterner::UNIT,
        has_env: false,
        cc: crate::CallConv::Go,
    }));
    let link = Linkage::External;
    let built = FuncBuilder::new(module, &symbol, sig, link, Span::default()).and_then(|b| {
        let mut cx = FuncCx::new(
            shared,
            &mut *memo,
            b,
            0,
            (&shared.no_body, &shared.no_facts),
        );
        let entry = cx.b.new_block();
        cx.b.switch_to(entry);
        for step in steps {
            let step = cx.function_named(step, sig)?;
            let callee = Operand::Const(Constant {
                kind: ConstantKind::Func(step),
                ty: sig,
            });
            let (nothing, next) = (cx.b.temp(TypeInterner::UNIT), cx.b.new_block());
            let unwind = crate::UnwindAction::Continue;
            cx.b.call(
                callee,
                Vec::new(),
                Place::local(nothing),
                Some(next),
                unwind,
            )?;
            cx.b.switch_to(next);
        }
        cx.b.ret()?;
        cx.b.finish().map(drop)
    });
    if let Err(error) = built {
        let message = format!("{symbol} could not be lowered: {error}");
        let named = shared.files.first().and_then(|(tree, _)| tree.package);
        if let Some(named) = named {
            memo.diagnostics
                .push(Diagnostic::new(Code::GirVerify, named.span, message));
        }
    }
}

/// Adds the function of a step of the start of the package to the
/// module: it gives variables of the package their first value.
fn lower_initializer<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    job: &'a Job,
    first: &'a gopp_sema::Initializer,
) {
    let (tree, source) = shared.files[job.file as usize];
    let span = tree.exprs[first.value].span;
    let report = |memo: &mut Memo<'a>, code: Code, message: String| {
        memo.diagnostics.push(Diagnostic::new(code, span, message));
    };
    let sig = match memo.signature(shared, &mut module.types, None, job.sig) {
        Ok(sig) => sig,
        Err(what) => {
            return report(
                memo,
                Code::GirUnsupported,
                format!("not supported yet: {what}"),
            );
        }
    };
    let at = source.line_col(span.lo);
    let loc = Span {
        file: job.file + 1,
        line: at.line,
        col: at.column,
    };
    let built = FuncBuilder::new(module, &job.symbol, sig, Linkage::External, loc).and_then(|b| {
        let mut cx = FuncCx::new(
            shared,
            &mut *memo,
            b,
            job.file,
            (shared.body(job), &job.facts),
        );
        cx.owner = Some((job, None));
        cx.initializer(&first.lhs, first.value)?;
        cx.b.drop_unreached();
        cx.b.finish().map(drop)
    });
    if let Err(error) = built {
        let message = format!("{} could not be lowered: {error}", job.symbol);
        report(memo, Code::GirVerify, message);
    }
}

/// Adds the function that makes the deferred calls of a function to
/// the module: the runtime calls it for a panic, and the function
/// itself where it is left (PAN-4).
fn lower_deferred<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    job: &'a Job,
    loc: Span,
    outlined: Outlined,
) -> Built {
    let byte = module.types.int(8, false);
    let bytes = module.types.ptr(byte);
    let sig = module.types.intern(Type::Func(crate::FuncType {
        params: [bytes].into(),
        ret: TypeInterner::UNIT,
        has_env: false,
        cc: crate::CallConv::C,
    }));
    let b = FuncBuilder::new(module, &outlined.symbol, sig, Linkage::Internal, loc)?;
    let body = shared.body(job);
    let mut cx = FuncCx::new(shared, &mut *memo, b, job.file, (body, &job.facts));
    cx.deferred_calls(&outlined)?;
    cx.b.drop_unreached();
    cx.b.finish().map(drop)
}

/// Adds the function of a function literal of the job to the module:
/// the code of its value, which takes an environment first.
pub(super) fn lower_literal<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    job: &'a Job,
    literal: ExprId,
) {
    let (tree, source) = shared.files[job.file as usize];
    let node = &tree.exprs[literal];
    let ExprKind::FuncLit { ty, body: block } = &node.kind else {
        return;
    };
    let Some(symbol) = memo.literal(shared, job, literal) else {
        return;
    };
    let report = |memo: &mut Memo<'a>, code: Code, message: String| {
        memo.diagnostics
            .push(Diagnostic::new(code, node.span, message));
    };
    // The type of the code is that of the first word of the value.
    let body = shared.body(job);
    let value = body.types.get(&literal).map(|typed| typed.ty);
    let value = value.and_then(|value| memo.lower_type(shared, &mut module.types, value).ok());
    let sig = value.and_then(|value| match module.types.get(value) {
        Type::Tuple(parts) => parts.first().copied(),
        _ => None,
    });
    let Some(sig) = sig else {
        let message = format!("{symbol} could not be lowered: its type is no function type");
        return report(memo, Code::GirVerify, message);
    };
    let at = source.line_col(node.span.lo);
    let loc = Span {
        file: job.file + 1,
        line: at.line,
        col: at.column,
    };
    let built = FuncBuilder::new(module, &symbol, sig, Linkage::Internal, loc).and_then(|b| {
        let mut cx = FuncCx::new(shared, &mut *memo, b, job.file, (body, &job.facts));
        let captures = body.captures.get(&literal).map(Vec::as_slice);
        let captures = captures.unwrap_or_default().iter();
        cx.captured = captures.map(|capture| capture.local).collect();
        cx.in_frame = job.facts.framed.contains(&literal);
        cx.owner = Some((job, Some(literal)));
        cx.function(None, *ty, block, 1)?;
        cx.b.drop_unreached();
        let outlined = cx.outlined();
        cx.b.finish()?;
        Ok(outlined)
    });
    let built = built.and_then(|outlined| match outlined {
        Some(outlined) if outlined.late => {
            memo.later.push(Later(job, Some(literal), loc, outlined));
            Ok(())
        }
        Some(outlined) => lower_deferred(shared, module, memo, job, loc, outlined),
        None => Ok(()),
    });
    if let Err(error) = built {
        let message = format!("{symbol} could not be lowered: {error}");
        report(memo, Code::GirVerify, message);
    }
}

/// The deferred calls of a function, or of a function literal, whose
/// loops over functions have `defer` statements in their bodies: they
/// are lowered when those bodies are.
pub(crate) struct Later<'a>(&'a Job, Option<ExprId>, Span, Outlined);

/// What a `defer` statement in the body of a loop over a function
/// calls, with the number of the statement in its function.
pub(crate) struct Late(usize, Pending);

/// Adds the function that makes the deferred calls of one function
/// that waited for the bodies of its loops; whether there was one.
pub(super) fn lower_later<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
) -> bool {
    let Some(Later(job, literal, loc, mut outlined)) = memo.later.pop() else {
        return false;
    };
    let late = memo.late.remove(&(std::ptr::from_ref(job), literal));
    for Late(at, call) in late.unwrap_or_default() {
        if let Some((_, known)) = outlined.calls.get_mut(at) {
            *known = Some(call);
        }
    }
    let symbol = outlined.symbol.clone();
    if let Err(error) = lower_deferred(shared, module, memo, job, loc, outlined) {
        let message = format!("{symbol} could not be lowered: {error}");
        let span = shared.files[job.file as usize].0.decl_nodes[job.decl].span;
        memo.diagnostics
            .push(Diagnostic::new(Code::GirVerify, span, message));
    }
    true
}

/// Adds the function of the body of a loop over a function to the
/// module: the code of the function value that the function which
/// walks is given, which takes an environment first.
pub(super) fn lower_ranged<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    job: &'a Job,
    stmt: gopp_syntax::ast::StmtId,
) {
    let (tree, source) = shared.files[job.file as usize];
    let span = tree.stmts[stmt].span;
    let known = memo.ranged.get(&(std::ptr::from_ref(job), stmt)).cloned();
    let (Some(ranged), Some(symbol)) = (known, memo.range_body(shared, job, stmt)) else {
        return;
    };
    let at = source.line_col(span.lo);
    let loc = Span {
        file: job.file + 1,
        line: at.line,
        col: at.column,
    };
    let body = shared.body(job);
    let built =
        FuncBuilder::new(module, &symbol, ranged.sig, Linkage::Internal, loc).and_then(|b| {
            let mut cx = FuncCx::new(shared, &mut *memo, b, job.file, (body, &job.facts));
            cx.captured = ranged.captured.clone();
            cx.in_frame = true;
            cx.owner = Some((job, ranged.within));
            cx.ranged_body(stmt, &ranged)?;
            cx.b.drop_unreached();
            cx.late_calls(job);
            cx.b.finish().map(drop)
        });
    if let Err(error) = built {
        let message = format!("{symbol} could not be lowered: {error}");
        memo.diagnostics
            .push(Diagnostic::new(Code::GirVerify, span, message));
    }
}

/// Adds the code of the function with the symbol `of` and the type
/// `ty` as a value to the module, under its symbol.
pub(super) fn lower_value<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    symbol: &str,
    (of, ty): (&str, CheckedTypeId),
    in_c: bool,
) {
    let value = memo.lower_type(shared, &mut module.types, ty);
    let sig = value.ok().and_then(|value| match module.types.get(value) {
        Type::Tuple(parts) => parts.first().copied(),
        _ => None,
    });
    let Some(sig) = sig else {
        return not_made(shared, memo, symbol, Ok(Err("a function type")));
    };
    let made = (symbol, sig, Linkage::LinkOnceOdr);
    lower_forward(shared, module, memo, made, (of, ty, in_c), 1);
}

/// Adds a function with the symbol, the type and the linkage of `made`
/// to the module, which calls the function `of` of the type `ty`, a
/// function of C if `in_c` is set, with what it is given after its
/// first `before` parameters.
pub(super) fn lower_forward<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    (symbol, sig, link): (&str, TypeId, Linkage),
    (of, ty, in_c): (&str, CheckedTypeId, bool),
    before: u32,
) {
    let built = FuncBuilder::new(module, symbol, sig, link, Span::default()).and_then(|b| {
        let mut cx = FuncCx::new(
            shared,
            &mut *memo,
            b,
            0,
            (&shared.no_body, &shared.no_facts),
        );
        let made = cx.value_body((of, ty, in_c), before)?;
        // What cannot be made has no body: its reason is reported.
        if made.is_err() {
            return Ok(made);
        }
        cx.b.drop_unreached();
        cx.b.finish()?;
        Ok(made)
    });
    not_made(shared, memo, symbol, built);
}

/// Adds the code of a method value or of a method expression of the
/// type `ty` to the module, under its symbol.
pub(super) fn lower_bound<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    symbol: &str,
    (made, ty): (super::Made, CheckedTypeId),
) {
    let (name, first, instance) = match made {
        super::Made::MethodValue(name, instance) => (name, None, instance),
        super::Made::MethodExpr { name, pointer } => (name, Some(pointer), None),
        _ => return,
    };
    let of = (ty, name, instance);
    let sig = FuncCx::bound_sig(shared, memo, &mut module.types, of, first);
    let sig = match sig {
        Ok(sig) => sig,
        Err(what) => return not_made(shared, memo, symbol, Ok(Err(what))),
    };
    let link = Linkage::LinkOnceOdr;
    let built = FuncBuilder::new(module, symbol, sig, link, Span::default()).and_then(|b| {
        let mut cx = FuncCx::new(
            shared,
            &mut *memo,
            b,
            0,
            (&shared.no_body, &shared.no_facts),
        );
        let made = cx.bound_body(of, first)?;
        // What cannot be made has no body: its reason is reported.
        if made.is_err() {
            return Ok(made);
        }
        cx.b.drop_unreached();
        cx.b.finish()?;
        Ok(made)
    });
    not_made(shared, memo, symbol, built);
}

/// Adds the glue of a kind for the type to the module, under its
/// symbol: one for all the packages that need it.
pub(super) fn lower_glue<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    symbol: &str,
    kind: Glue,
    ty: CheckedTypeId,
) {
    let sig = super::glue::sig(&mut module.types, kind);
    let link = Linkage::LinkOnceOdr;
    let built = FuncBuilder::new(module, symbol, sig, link, Span::default()).and_then(|b| {
        let mut cx = FuncCx::new(
            shared,
            &mut *memo,
            b,
            0,
            (&shared.no_body, &shared.no_facts),
        );
        let made = match kind {
            Glue::Clone | Glue::Drop => cx.walk_body(kind, ty)?,
            Glue::Eq => cx.eq_body(ty)?,
            Glue::Hash => cx.hash_body(ty)?,
        };
        // What cannot be made has no body: its reason is reported.
        if made.is_err() {
            return Ok(made);
        }
        cx.b.drop_unreached();
        cx.b.finish()?;
        Ok(made)
    });
    not_made(shared, memo, symbol, built);
}

/// Adds what a table of methods holds for a method of the declared type
/// `ty` to the module, under its symbol: the method `name`, for the
/// table of the type or of the pointers to it.
pub(super) fn lower_thunk<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    symbol: &str,
    (ty, name, pointer): (CheckedTypeId, Spur, bool),
) {
    let sig = match super::iface::thunk_sig(shared, memo, &mut module.types, (ty, name, pointer)) {
        Ok(sig) => sig,
        Err(what) => return not_made(shared, memo, symbol, Ok(Err(what))),
    };
    let link = Linkage::LinkOnceOdr;
    let built = FuncBuilder::new(module, symbol, sig, link, Span::default()).and_then(|b| {
        let mut cx = FuncCx::new(
            shared,
            &mut *memo,
            b,
            0,
            (&shared.no_body, &shared.no_facts),
        );
        let made = cx.thunk_body(ty, name, pointer)?;
        // What cannot be made has no body: its reason is reported.
        if made.is_err() {
            return Ok(made);
        }
        cx.b.drop_unreached();
        cx.b.finish()?;
        Ok(made)
    });
    not_made(shared, memo, symbol, built);
}

/// Adds what a type table has for the method `name` of the type `ty`
/// to the module, under its symbol.
pub(super) fn lower_adjusted<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    symbol: &str,
    (ty, name, by): (CheckedTypeId, Spur, i64),
) {
    let sig = match shared.method(ty, name) {
        Some(method) => super::table::slot_sig(shared, memo, &mut module.types, method.sig),
        None => Err("a method that its type does not have"),
    };
    let sig = match sig {
        Ok(sig) => sig,
        Err(what) => return not_made(shared, memo, symbol, Ok(Err(what))),
    };
    let link = Linkage::LinkOnceOdr;
    let built = FuncBuilder::new(module, symbol, sig, link, Span::default()).and_then(|b| {
        let mut cx = FuncCx::new(
            shared,
            &mut *memo,
            b,
            0,
            (&shared.no_body, &shared.no_facts),
        );
        let made = cx.adjusted_body(ty, name, by)?;
        // What cannot be made has no body: its reason is reported.
        if made.is_err() {
            return Ok(made);
        }
        cx.b.drop_unreached();
        cx.b.finish()?;
        Ok(made)
    });
    not_made(shared, memo, symbol, built);
}

/// What asked for a function of the package saw that it can be made:
/// one that is not made is a fault of the lowering, which is reported
/// at the start of the package.
fn not_made(
    shared: &Shared<'_>,
    memo: &mut Memo<'_>,
    symbol: &str,
    built: Built<Result<(), &'static str>>,
) {
    let failed = match built {
        Ok(Ok(())) => return,
        Ok(Err(what)) => what.to_owned(),
        Err(error) => error.to_string(),
    };
    let (tree, _) = shared.files[0];
    let span = tree.decl_nodes.iter().next().map(|(_, decl)| decl.span);
    if let Some(span) = tree.package.map(|name| name.span).or(span) {
        let message = format!("{symbol} could not be lowered: {failed}");
        memo.diagnostics
            .push(Diagnostic::new(Code::GirVerify, span, message));
    }
}

/// What a temporary that owns is known by: its slot, and which part of
/// it if it is one of several results.
fn temp_key(place: &Place) -> Option<(LocalId, u32)> {
    match &place.projections[..] {
        [] => Some((place.local, u32::MAX)),
        [part] => match part.kind {
            ProjectionKind::Field(at) => Some((place.local, at)),
            _ => None,
        },
        _ => None,
    }
}

impl<'a, 'm> FuncCx<'a, 'm> {
    /// What lowers one function of the file with the number `file`
    /// from what the checker found in it.
    fn new(
        shared: &'a Shared<'a>,
        memo: &'m mut Memo<'a>,
        b: FuncBuilder<'m>,
        file: u32,
        (body, facts): (&'a Body, &'a Facts),
    ) -> Self {
        let (tree, source) = shared.files[file as usize];
        Self {
            shared,
            memo,
            b,
            tree,
            source,
            file,
            body,
            facts,
            lent: FxHashSet::default(),
            copied: FxHashSet::default(),
            lent_params: FxHashSet::default(),
            worked_params: FxHashSet::default(),
            unboxed: FxHashMap::default(),
            parts: FxHashMap::default(),
            cells: FxHashMap::default(),
            captured: Vec::new(),
            in_frame: false,
            ranged: None,
            late_defers: false,
            deferring: None,
            cell_vars: FxHashMap::default(),
            vars: FxHashMap::default(),
            scopes: Vec::new(),
            labels: FxHashMap::default(),
            targets: Vec::new(),
            results: Vec::new(),
            label: None,
            effects: FxHashMap::default(),
            reported: FxHashSet::default(),
            reused: None,
            aliases: FxHashSet::default(),
            views: Vec::new(),
            outer: FxHashMap::default(),
            temps: Vec::new(),
            owning: FxHashMap::default(),
            borrowed: FxHashSet::default(),
            handed: FxHashSet::default(),
            defers: Vec::new(),
            jumps: false,
            fails: false,
            owner: None,
            frame: None,
            deferred: None,
            lists: Vec::new(),
            extendable: FxHashMap::default(),
            declared: None,
            kept_lists: Vec::new(),
            region: None,
            owners: 0,
            unwind_head: None,
            temp_heads: Vec::new(),
            temp_base: None,
            ending: 0,
            quiet: false,
            frame_block: None,
            results_owned: 0..0,
            hidden: Vec::new(),
            moved: Vec::new(),
        }
    }
}

impl FuncCx<'_, '_> {
    /// The body of a function whose first parameter of the source has
    /// the number `first`: an environment comes before the parameters
    /// of a function literal.
    /// The body of the function that gives the variables `lhs` of the
    /// package their first value: `value` is worked out, and each
    /// variable is the owner of what it gets. Nothing ends them
    /// (INI-5).
    fn initializer(&mut self, lhs: &[gopp_sema::ObjId], value: ExprId) -> Built {
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        self.scopes.push(Scope::default());
        let span = self.tree.exprs[value].span;
        self.at(span);
        let mark = self.temps.len();
        let package = self.home();
        // A variable named `_` has no place: its value ends here.
        let named = |object: &gopp_sema::ObjId| package.object(*object).name != self.shared.blank;
        let mut wanted = Vec::with_capacity(lhs.len());
        for object in lhs {
            wanted.push(match named(object) {
                true => Some(self.lower(package.object(*object).ty, span)),
                false => None,
            });
        }
        let values = self.owned_values(&[value], &wanted)?;
        for (object, value) in lhs.iter().zip(values) {
            if named(object) {
                let key = super::CalleeKey::Object(*object);
                let place = self.global(key, package, *object, span)?;
                self.store(place, value)?;
            }
        }
        self.drop_temps(mark)?;
        self.exit_scopes(0)?;
        self.b.ret()
    }

    fn function(
        &mut self,
        receiver: Option<&Param>,
        ty: ExprId,
        block: &Block,
        first: u32,
    ) -> Built {
        let ExprKind::FuncType {
            params,
            results,
            fails,
        } = &self.tree.exprs[ty].kind
        else {
            return Ok(());
        };
        self.fails = *fails;
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        self.scopes.push(Scope::default());
        self.open_env()?;
        for (at, param) in receiver.into_iter().chain(params).enumerate() {
            let declared = param
                .name
                .and_then(|name| self.body.defs.get(&name.span.lo));
            if let (Some(name), Some(&local)) = (param.name, declared) {
                let slot = self.b.param(first + at as u32);
                self.b
                    .set_name(slot, self.memo.name(self.shared, name.name));
                self.vars.insert(local, slot);
                // A parameter that a function literal uses lives in a
                // cell, which has the value that the caller gave.
                if self.facts.celled.contains(&local) {
                    let ty = self.b.place_ty(&Place::local(slot));
                    let var = self.declare_cell(local, ty)?;
                    let place = self.var_place(var)?;
                    let given = Operand::Copy(Place::local(slot));
                    self.b.assign(place, Rvalue::Use(given))?;
                    continue;
                }
                // A parameter that the function only reads is the
                // caller's; any other is the function's own: the caller
                // gave a copy or the value itself.
                match self.facts.lent.get(at) {
                    Some(true) => drop(self.lent_params.insert(slot)),
                    _ => self.owns(local, slot)?,
                }
                if self.facts.worked.get(at) == Some(&true) {
                    self.worked_params.insert(slot);
                }
            }
        }
        let results_from = self.scopes.last().map_or(0, |scope| scope.owned.len());
        for result in results {
            let declared = result
                .name
                .and_then(|name| self.body.defs.get(&name.span.lo));
            let var = match declared {
                Some(&local) => {
                    let var = self.declare(local)?;
                    let place = self.var_place(var)?;
                    let zero = self.zero(self.b.place_ty(&place));
                    self.b.assign(place, Rvalue::Use(zero))?;
                    Some(var)
                }
                None => None,
            };
            self.results.push(var);
        }
        // The error of a function that can fail has no variable.
        if self.fails {
            self.results.push(None);
        }
        let results_to = self.scopes.last().map_or(0, |scope| scope.owned.len());
        let parts = self.result_parts();
        let mut next = first + (usize::from(receiver.is_some()) + params.len()) as u32;
        for &part in &parts {
            let view = super::types::is_view(self.b.types(), part).then(|| {
                next += 1;
                self.b.param(next - 1)
            });
            self.views.push(view);
        }
        let mut lists = 0;
        self.find_defers(&block.stmts, None, &mut lists);
        // In a function with a label any statement may run again.
        if self.jumps {
            lists = 1;
            for deferred in &mut self.defers {
                deferred.list = Some(0);
            }
        }
        if !self.defers.is_empty() {
            let owner = self.owner;
            self.deferred =
                owner.and_then(|(job, literal)| self.memo.deferred(self.shared, job, literal));
        }
        // A function with `defer` statements starts by saying that
        // none of them ran. What they keep is known when the body is
        // lowered, so the start is written last.
        let start = match self.deferred.clone() {
            None => None,
            Some(symbol) => {
                // A deferred call may stop a panic after a `return`
                // gave the results their values: each result has a
                // variable, which has them then.
                let checked = self.result_types();
                for (at, &part) in parts.iter().enumerate().take(self.results.len()) {
                    if self.results[at].is_none() {
                        let var = self.b.temp(part);
                        let zero = self.zero(part);
                        self.b.assign(Place::local(var), Rvalue::Use(zero))?;
                        self.results[at] = Some(var);
                        if !self.flat(part)
                            && let Some(ends) =
                                self.ends_of(checked.get(at).copied(), block.span)?
                        {
                            self.hidden.push((var, ends));
                        }
                    }
                }
                // The first block itself, unless a cell of a parameter
                // or of a result was made in it.
                let body = self.b.new_block();
                let first = match self.b.current() {
                    Some(current) if current == entry => entry,
                    _ => {
                        let first = self.b.new_block();
                        self.b.goto(first)?;
                        first
                    }
                };
                let bytes = self.bytes_ty();
                let run_ty = self.glue_sig(Glue::Drop);
                let run = self.function_named(&symbol, run_ty)?;
                let pinned = self.pinned_results();
                let count = self.defers.len() + pinned.len();
                let types = self.b.types();
                let node = types.tuple(&[bytes, run_ty, bytes, bytes]);
                let records = types.tuple(&vec![bytes; count]);
                self.frame = Some(Frame {
                    node: (!self.shared.aborts).then(|| self.b.temp(node)),
                    records: self.b.temp(records),
                    run,
                });
                self.lists = (0..lists).map(|_| self.b.temp(bytes)).collect();
                // The blocks of the outermost scope stand on the block
                // of the frame from here on, which ends the results.
                self.results_owned = results_from..results_to;
                if let Some(scope) = self.scopes.first_mut() {
                    scope.unwind = Chain::default();
                }
                self.owners += 1;
                self.b.switch_to(body);
                Some((first, body, self.b.loc()))
            }
        };
        self.stmts(&block.stmts)?;
        // The end of the body: a function with results never comes
        // here, as the checker saw.
        self.at_end(block.span);
        match results.is_empty() {
            // A function that can fail returns no error here.
            true if self.fails => {
                self.leave(block.span, None)?;
                self.b.unreachable()?;
            }
            true => {
                self.run_defers()?;
                self.exit_scopes(0)?;
                self.b.ret()?;
            }
            false => self.b.unreachable()?,
        }
        let (Some((entry, body, loc)), Some(frame)) = (start, self.frame) else {
            return Ok(());
        };
        self.b.switch_to(entry);
        self.b.set_loc(loc);
        // No statement ran; the runtime knows the function from here
        // on. No panic starts in this block.
        self.quiet = true;
        let bytes = self.bytes_ty();
        let empties = self
            .lists
            .iter()
            .chain(&self.kept_lists)
            .chain(&self.declared);
        for slot in empties.copied().collect::<Vec<_>>() {
            let empty = self.zero(self.b.place_ty(&Place::local(slot)));
            self.b.assign(Place::local(slot), Rvalue::Use(empty))?;
        }
        let mut addresses = Vec::new();
        for at in 0..self.defers.len() {
            let call = self.defers[at].call.as_ref();
            let record = call.and_then(|call| Some((call.record?, call.ty)));
            let list = self.defers[at].list.and_then(|list| self.lists.get(list));
            addresses.push(match (list, record) {
                // A statement of a list is found by its list.
                (Some(&list), _) => self.record(Place::local(list))?,
                (None, Some((record, ty))) => {
                    let record = Place::local(record);
                    let zero = Rvalue::Use(self.zero(ty));
                    self.b.assign(record.clone(), zero)?;
                    self.record(record)?
                }
                // A statement that was refused keeps nothing.
                (None, None) => self.zero(bytes),
            });
        }
        for var in self.pinned_results() {
            addresses.push(self.record(Place::local(var))?);
        }
        let records = Place::local(frame.records);
        let all = Rvalue::Aggregate(crate::AggKind::Tuple, addresses);
        self.b.assign(records.clone(), all)?;
        // The runtime knows nothing of the function where no panic
        // makes its deferred calls.
        let Some(node) = frame.node else {
            self.quiet = false;
            return self.b.goto(body);
        };
        let run = Operand::Const(Constant {
            kind: ConstantKind::Func(frame.run),
            ty: self.glue_sig(Glue::Drop),
        });
        let fields = vec![
            self.zero(bytes),
            run,
            self.record(records)?,
            self.zero(bytes),
        ];
        let node = Place::local(node);
        self.b.assign(
            node.clone(),
            Rvalue::Aggregate(crate::AggKind::Tuple, fields),
        )?;
        let node = self.record(node)?;
        let stopped = self.runtime(
            "_gopp_defer_enter",
            &[bytes],
            TypeInterner::BOOL,
            vec![node],
        )?;
        self.quiet = false;
        let after = self.b.new_block();
        self.b.switch(stopped, vec![(0, body)], after)?;
        // A deferred call stopped a panic: the function returns to its
        // caller with what its results have (PAN-1). What it owned
        // where the panic left it has ended: it ran its cleanup blocks
        // when it came back here.
        self.b.switch_to(after);
        let named = self.results.clone();
        let given = self.results_now(&named, &parts, &[])?;
        self.give(given)?;
        // What is left of the results ends: the cell of a result that a
        // function literal uses.
        for at in self.results_owned.clone().rev() {
            if let Some((place, ends)) = self.scopes.first().and_then(|scope| scope.owned.get(at)) {
                let (place, ends) = (place.clone(), *ends);
                self.end(place, ends)?;
            }
        }
        self.b.ret()
    }

    /// The variables of the results whose addresses the runtime is
    /// given: those that are slots of the function and take room.
    fn pinned_results(&mut self) -> Vec<LocalId> {
        let vars: Vec<LocalId> = self.results.iter().flatten().copied().collect();
        let mut pinned = Vec::new();
        for var in vars {
            let ty = self.b.place_ty(&Place::local(var));
            let layout = self.b.types().layout(ty, self.shared.layout);
            if !self.cell_vars.contains_key(&var) && layout.is_ok_and(|layout| layout.size > 0) {
                pinned.push(var);
            }
        }
        pinned
    }

    /// The deferred calls of the function, for the function that makes
    /// them; none for a function without `defer` statements.
    fn outlined(&mut self) -> Option<Outlined> {
        let symbol = self.deferred.take()?;
        let calls: Vec<_> = self
            .defers
            .drain(..)
            .map(|deferred| (deferred.list, deferred.call))
            .collect();
        let count = calls.len() + self.pinned_results().len();
        Some(Outlined {
            symbol,
            late: self.late_defers,
            count,
            calls,
        })
    }

    /// The `defer` statements of the statements and of the blocks in
    /// them, in the order of the source. Those of an outermost loop
    /// share a list, `list`; `lists` counts the lists.
    fn find_defers(&mut self, stmts: &[StmtId], list: Option<usize>, lists: &mut usize) {
        let tree = self.tree;
        for &stmt in stmts {
            self.memo.steps += 1;
            match &tree.stmts[stmt].kind {
                StmtKind::Defer(_) => self.defers.push(Deferred {
                    stmt,
                    list,
                    call: None,
                }),
                StmtKind::Labeled { stmt, .. } => {
                    self.jumps = true;
                    self.find_defers(&[*stmt], list, lists);
                }
                StmtKind::Branch {
                    kind: BranchKind::Goto,
                    ..
                } => self.jumps = true,
                StmtKind::Block(block) => {
                    deeper(|| self.find_defers(&block.stmts, list, lists));
                }
                StmtKind::If {
                    then, otherwise, ..
                } => {
                    deeper(|| self.find_defers(&then.stmts, list, lists));
                    self.find_defers(otherwise.as_slice(), list, lists);
                }
                StmtKind::For { body, .. } | StmtKind::Range { body, .. } => {
                    // A loop in no loop has a list of its own, if it
                    // has a `defer` statement at all.
                    let before = self.defers.len();
                    deeper(|| self.find_defers(&body.stmts, list.or(Some(*lists)), lists));
                    if list.is_none() && self.defers.len() > before {
                        *lists += 1;
                    }
                }
                StmtKind::Switch { clauses, .. } => {
                    for clause in clauses {
                        deeper(|| self.find_defers(&clause.body, list, lists));
                    }
                }
                StmtKind::Select { clauses } => {
                    for clause in clauses {
                        deeper(|| self.find_defers(&clause.body, list, lists));
                    }
                }
                _ => {}
            }
        }
    }

    /// The place with the first record of the list. The body of a
    /// loop over a function has the list of the function with the
    /// loop, which all its `defer` statements share.
    pub fn defer_list(&self, list: usize) -> Option<Place> {
        match &self.ranged {
            Some(ranged) => ranged.list.clone(),
            None => self.lists.get(list).map(|&list| Place::local(list)),
        }
    }

    /// What the `defer` statements of the body of a loop over a
    /// function call, each with its number in the function with the
    /// loop, whose deferred calls are lowered after this.
    fn late_calls(&mut self, job: &Job) {
        let Some((base, within)) = self
            .ranged
            .as_ref()
            .map(|ranged| (ranged.base, ranged.owner))
        else {
            return;
        };
        let calls = self.defers.drain(..).enumerate();
        let calls: Vec<Late> = calls
            .filter_map(|(at, deferred)| Some(Late(base + at, deferred.call?)))
            .collect();
        let key = (std::ptr::from_ref(job), within);
        self.memo.late.entry(key).or_default().extend(calls);
    }

    /// The function is left: the calls of its `defer` statements are
    /// made first (PAN-4), and the runtime knows the function no more.
    pub fn run_defers(&mut self) -> Built {
        let Some(frame) = self.frame else {
            return Ok(());
        };
        let records = self.record(Place::local(frame.records))?;
        self.call_glue(frame.run, records)?;
        let Some(node) = frame.node else {
            return Ok(());
        };
        let bytes = self.bytes_ty();
        let node = self.record(Place::local(node))?;
        self.runtime_call("_gopp_defer_pop", &[bytes], vec![node])
    }

    /// The body of the function that makes the deferred calls of a
    /// function: the calls of the statements that ran, the last first.
    /// A statement that runs once at most runs after those before it in
    /// the source, so the order of the source turned around is theirs;
    /// the statements of a loop are in the list of the loop, which is
    /// gone through where the loop is among them. A statement has not
    /// run any more once its call is started: a panic in the call makes
    /// the calls that are left, and not this one again.
    fn deferred_calls(&mut self, outlined: &Outlined) -> Built {
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        let bytes = self.bytes_ty();
        let records = self.b.types().tuple(&vec![bytes; outlined.count]);
        let records = self.b.types().ptr(records);
        let given = Operand::Copy(Place::local(self.b.param(0)));
        let records =
            self.b
                .assign_temp(Rvalue::Cast(crate::CastKind::PtrToPtr, given, records))?;
        let records = self.b.deref(&Place::local(records))?;
        let mut gone_through = FxHashSet::default();
        for (at, (list, pending)) in outlined.calls.iter().enumerate().rev() {
            self.memo.steps += 1;
            let address = Operand::Copy(self.b.field(&records, at as u32)?);
            if let Some(list) = list {
                if gone_through.insert(*list) {
                    self.deferred_list(address, *list, &outlined.calls)?;
                }
                continue;
            }
            // A statement that was refused keeps nothing.
            let Some(pending) = pending else {
                continue;
            };
            let record = self.b.types().ptr(pending.ty);
            let record =
                self.b
                    .assign_temp(Rvalue::Cast(crate::CastKind::PtrToPtr, address, record))?;
            let record = self.b.deref(&Place::local(record))?;
            let flag = self.b.field(&record, 0)?;
            let [call, done] = [(); 2].map(|()| self.b.new_block());
            self.b
                .switch(Operand::Copy(flag.clone()), vec![(0, done)], call)?;
            self.b.switch_to(call);
            let no = self.b.const_bool(false);
            self.b.assign(flag, Rvalue::Use(no))?;
            self.deferred_call(pending, &record, 1, None)?;
            self.b.goto(done)?;
            self.b.switch_to(done);
        }
        self.b.ret()
    }

    /// The calls of the records of a list, from the one made last on.
    /// `address` is that of the slot with the first record. A record is
    /// taken off the list before its call, and its cell is let go after
    /// it.
    fn deferred_list(
        &mut self,
        address: Operand,
        list: usize,
        calls: &[(Option<usize>, Option<Pending>)],
    ) -> Built {
        let bytes = self.bytes_ty();
        let word = self.b.types().int(64, false);
        let slot = self.b.types().ptr(bytes);
        let first = self
            .b
            .assign_temp(Rvalue::Cast(crate::CastKind::PtrToPtr, address, slot))?;
        let first = self.b.deref(&Place::local(first))?;
        let [again, taken, over, never] = [(); 4].map(|()| self.b.new_block());
        self.b.goto(again)?;
        self.b.switch_to(again);
        let cell = self.slot_of(Rvalue::Use(Operand::Copy(first.clone())))?;
        let any = Rvalue::Binary(
            crate::BinOp::Ne,
            Operand::Copy(cell.clone()),
            self.zero(bytes),
        );
        let any = Operand::Copy(Place::local(self.b.assign_temp(any)?));
        self.b.switch(any, vec![(0, over)], taken)?;
        self.b.switch_to(taken);
        // Every record starts with the record after it and the number
        // of its statement.
        let start = self.b.types().tuple(&[bytes, word]);
        let start = self.cell_type(start);
        let start = self.b.types().ptr(start);
        let start = Rvalue::Cast(
            crate::CastKind::PtrToPtr,
            Operand::Copy(cell.clone()),
            start,
        );
        let start = Place::local(self.b.assign_temp(start)?);
        let start = self.b.deref(&start)?;
        let start = self.b.field(&start, 1)?;
        let next = Operand::Copy(self.b.field(&start, 0)?);
        self.b.assign(first, Rvalue::Use(next))?;
        let which = self.b.field(&start, 1)?;
        let which = Operand::Copy(self.slot_of(Rvalue::Use(Operand::Copy(which)))?);
        let mut cases = Vec::new();
        let of_list = (calls.iter().enumerate()).filter(|(_, (of, _))| *of == Some(list));
        for (at, (_, pending)) in of_list {
            self.memo.steps += 1;
            let Some(pending) = pending else {
                continue;
            };
            let call = self.b.new_block();
            cases.push((at as u64, call));
            self.b.switch_to(call);
            let record = self.cell_type(pending.ty);
            let record = self.b.types().ptr(record);
            let record = Rvalue::Cast(
                crate::CastKind::PtrToPtr,
                Operand::Copy(cell.clone()),
                record,
            );
            let record = Place::local(self.b.assign_temp(record)?);
            let record = self.b.deref(&record)?;
            let record = self.b.field(&record, 1)?;
            self.deferred_call(pending, &record, 2, Some(cell.clone()))?;
            self.b.goto(again)?;
        }
        self.b.switch_to(taken);
        self.b.switch(which, cases, never)?;
        self.b.switch_to(never);
        self.b.unreachable()?;
        self.b.switch_to(over);
        Ok(())
    }

    /// The call of a statement, with what its record keeps from the
    /// part `first` on. The cell of the record, if it has one, is let
    /// go after the call.
    fn deferred_call(
        &mut self,
        pending: &Pending,
        record: &Place,
        first: u32,
        cell: Option<Place>,
    ) -> Built {
        let mut slots = Vec::with_capacity(pending.slots.len());
        for slot in 0..pending.slots.len() {
            slots.push(self.b.field(record, slot as u32 + first)?);
        }
        // The address of the result with the error is no argument.
        let sets = pending.marked.as_ref().is_some_and(|marked| marked.sets);
        let error_at = match sets {
            true => slots.pop(),
            false => None,
        };
        let given: Vec<Operand> = slots.iter().cloned().map(Operand::Copy).collect();
        let around = self.b.loc();
        self.at(pending.span);
        // What the call leaves to the statement ends after the call,
        // also where a panic leaves it: a parameter that is only read,
        // what `print` writes, the value that is called.
        let lent = |at: usize| match &pending.what {
            Waits::Call(called) => called.lent.get(at) == Some(&true),
            Waits::Print(_) | Waits::Delete(_) | Waits::Clear(_) | Waits::Nothing => true,
            Waits::Value(_) => at == 0,
            // The runtime has the value of the panic.
            Waits::Panic => false,
        };
        self.open_scope();
        if let Some(cell) = cell {
            self.hold(cell, Ends::Cell(None));
        }
        for (at, (slot, ends)) in slots.iter().zip(&pending.slots).enumerate() {
            if let (true, Some(ends)) = (lent(at), *ends) {
                self.hold(slot.clone(), ends);
            }
        }
        let after = self.b.new_block();
        match &pending.what {
            Waits::Call(called) => {
                let callee = Operand::Const(Constant {
                    kind: ConstantKind::Func(called.func),
                    ty: called.sig,
                });
                let result = Place::local(self.b.temp(called.ret));
                let unwind = self.unwind()?;
                self.b
                    .call(callee, given, result.clone(), Some(after), unwind)?;
                self.b.switch_to(after);
                if let Some(marked) = &pending.marked {
                    self.deferred_error(pending.span, marked, &result, error_at)?;
                } else {
                    self.end_results(result, &pending.result)?;
                }
            }
            Waits::Print(line) => {
                self.b.goto(after)?;
                self.b.switch_to(after);
                let values = given.into_iter().map(|value| (value, pending.span));
                self.print_values(*line, values.collect())?;
            }
            Waits::Value(code_ty) => {
                self.b.goto(after)?;
                self.b.switch_to(after);
                if let Some(value) = slots.first() {
                    let (ret, result) = self.call_value(value, *code_ty, given)?;
                    if let Some(marked) = &pending.marked {
                        self.deferred_error(pending.span, marked, &result, error_at)?;
                    } else if ret != TypeInterner::UNIT {
                        self.end_results(result, &pending.result)?;
                    }
                }
            }
            Waits::Panic => {
                self.b.goto(after)?;
                self.b.switch_to(after);
                if let Some(value) = slots.first() {
                    self.panic_with(pending.span, value.clone())?;
                }
            }
            Waits::Delete(of) | Waits::Clear(of) => {
                self.b.goto(after)?;
                self.b.switch_to(after);
                // The map that its place holds now.
                if let Some(place) = slots.first() {
                    let map = self.b.deref(place)?;
                    let map = Operand::Copy(self.slot_of(Rvalue::Use(Operand::Copy(map)))?);
                    let key = match slots.get(1) {
                        Some(key) => Some(self.record(key.clone())?),
                        None => None,
                    };
                    self.map_forget(map, of, key)?;
                }
            }
            Waits::Nothing => {
                self.b.goto(after)?;
                self.b.switch_to(after);
            }
        }
        self.close_scope(pending.span)?;
        self.b.set_loc(around);
        Ok(())
    }

    /// What a deferred call with `!` gave, in `result`, ends with an
    /// error or without (ERR-11a). In a function that can fail the
    /// error becomes that of the function, whose result is at the
    /// address in `error_at`, if the function has none yet and no panic
    /// runs the call; in any other function the error is a panic.
    fn deferred_error(
        &mut self,
        span: SourceSpan,
        marked: &Marked,
        result: &Place,
        error_at: Option<Place>,
    ) -> Built {
        let error = match self.kind(self.b.place_ty(result)) {
            Type::Tuple(parts) => self.b.field(result, parts.len() as u32 - 1)?,
            _ => result.clone(),
        };
        let error = self.slot_of(Rvalue::Use(Operand::Copy(error)))?;
        let bytes = self.bytes_ty();
        let set = |this: &mut Self, value: &Place| -> Built<Operand> {
            let first = Operand::Copy(this.b.field(value, 0)?);
            let set = Rvalue::Binary(crate::BinOp::Ne, first, this.zero(bytes));
            Ok(Operand::Copy(Place::local(this.b.assign_temp(set)?)))
        };
        let failed = set(self, &error)?;
        let [fails, done] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(failed, vec![(0, done)], fails)?;
        self.b.switch_to(fails);
        self.b.set_cold(fails);
        match error_at {
            None => self.must_with(span, &marked.shown, &marked.place, error)?,
            Some(at) => {
                let own = self.b.deref(&at)?;
                let [asks, stores, drops] = [(); 3].map(|()| self.b.new_block());
                let panics = self.runtime("_gopp_panicking", &[], TypeInterner::BOOL, vec![])?;
                self.b.switch(panics, vec![(0, asks)], drops)?;
                self.b.switch_to(asks);
                let has = set(self, &own)?;
                self.b.switch(has, vec![(0, stores)], drops)?;
                self.b.switch_to(stores);
                self.traced(&error, &marked.place)?;
                self.b
                    .assign(own, Rvalue::Use(Operand::Copy(error.clone())))?;
                self.b.goto(done)?;
                self.b.switch_to(drops);
                if let Some(ends) = marked.ends {
                    self.end(error, ends)?;
                }
            }
        }
        self.b.goto(done)?;
        self.b.switch_to(done);
        Ok(())
    }

    // ---- places in the source

    pub fn loc(&self, span: SourceSpan) -> Span {
        let at = self.source.line_col(span.lo);
        Span {
            file: self.file + 1,
            line: at.line,
            col: at.column,
        }
    }

    /// What is written next comes from `span`.
    pub fn at(&mut self, span: SourceSpan) {
        let loc = self.loc(span);
        self.b.set_loc(loc);
    }

    /// What is written next comes from the closing brace of `span`.
    pub fn at_end(&mut self, span: SourceSpan) {
        let end = SourceSpan {
            lo: span.hi.saturating_sub(1).max(span.lo),
            ..span
        };
        self.at(end);
    }

    /// Says once that what is at `span` is not supported yet.
    pub fn unsupported(&mut self, span: SourceSpan, what: &str) {
        // What is wrong with a declaration is said there.
        if what != SAID && self.reported.insert((span.lo, span.hi)) {
            let message = format!("not supported yet: {what}");
            let diagnostic = Diagnostic::new(Code::GirUnsupported, span, message);
            self.memo.diagnostics.push(diagnostic);
        }
    }

    /// Whether a value of the type cannot be copied. The program needs
    /// a copy of `what` here that it does not write: that is said once
    /// (`E0452`), with the part of the type that has no copy, and with
    /// the note, if there is one.
    pub fn uncopied(
        &mut self,
        span: SourceSpan,
        ty: CheckedTypeId,
        what: &str,
        note: Option<(SourceSpan, String)>,
    ) -> bool {
        let shared = self.shared;
        let Some((part, why)) = shared.types.not_copied(ty, true) else {
            return false;
        };
        if self.reported.insert((span.lo, span.hi)) {
            let message = format!(
                "cannot copy {what}: {}",
                why.text(&shared.shown(part), true)
            );
            // In an instance the type is what a type argument makes it.
            let instance = self.owner.filter(|(job, _)| shared.is_instance(job));
            let code = match instance {
                Some(_) => Code::InstImplicitClone,
                None => Code::OwnImplicitClone,
            };
            let mut diagnostic = Diagnostic::new(code, span, message);
            if let Some((at, note)) = note {
                diagnostic = diagnostic.with_note(Code::NoteCapturedHere, Some(at), note);
            }
            if let Some((job, _)) = instance {
                diagnostic = shared.led_to(job, diagnostic);
            }
            self.memo.diagnostics.push(diagnostic);
        }
        true
    }

    // ---- types

    /// The type of GIR for a type of the checker; the empty type, with
    /// a report, for one that is not supported.
    pub fn lower(&mut self, ty: CheckedTypeId, span: SourceSpan) -> TypeId {
        // The table of the checker is shared by all runs: it is asked
        // once for each type.
        let lowered = match self.memo.expr_types.get(&ty) {
            Some(&known) => known,
            None => {
                let typed = self.shared.types.default_type(ty);
                let lowered = match self.shared.types.is_untyped(typed) {
                    true => Ok(TypeInterner::UNIT),
                    false => self.memo.lower_type(self.shared, self.b.types(), typed),
                };
                self.memo.expr_types.insert(ty, lowered);
                lowered
            }
        };
        match lowered {
            Ok(lowered) => lowered,
            Err(what) => {
                self.unsupported(span, what);
                TypeInterner::UNIT
            }
        }
    }

    pub fn kind(&mut self, ty: TypeId) -> Type {
        self.b.types().get(ty).clone()
    }

    /// The zero of the type: the value that a new value starts as. It
    /// is all zero bytes but for the addresses of the tables of the
    /// values with tables that it holds.
    pub fn zero(&mut self, ty: TypeId) -> Operand {
        let kind = match self.blank(ty) {
            Some(blank) => ConstantKind::Value(blank),
            None => ConstantKind::Zero,
        };
        Operand::Const(Constant { kind, ty })
    }

    // ---- variables and scopes

    pub fn open_scope(&mut self) {
        // A block of the function itself: the variables in it that
        // `defer` statements borrow have a list of their own, which
        // ends where the variables of the function end that were
        // declared before the block. Every call in the block may be
        // left by a panic while the list has variables of an earlier
        // pass, so the list is there before the block.
        if self.scopes.len() == 1 && self.frame.is_some() && !self.facts.extended.is_empty() {
            let bytes = self.bytes_ty();
            let list = self.b.temp(bytes);
            self.kept_lists.push(list);
            self.region = Some(list);
            self.hold(Place::local(list), Ends::Extended);
        }
        self.scopes.push(Scope::default());
        self.owners += 1;
    }

    /// Ends the innermost scope, which the source ends at the end of
    /// `span`: its variables are dead from here.
    pub fn close_scope(&mut self, span: SourceSpan) -> Built {
        self.at_end(span);
        self.exit_scopes(self.scopes.len() - 1)?;
        if self.scopes.pop().is_some_and(|scope| scope.debug) {
            self.b.pop_scope();
        }
        self.owners += 1;
        Ok(())
    }

    pub fn depth(&self) -> usize {
        self.scopes.len()
    }

    /// The variables of the scopes from `depth` on are dead, the
    /// innermost and the last declared first, and their walks of maps
    /// are over. The scopes stay open: this is a way out of them, not
    /// their end.
    pub fn exit_scopes(&mut self, depth: usize) -> Built {
        for scope in (depth..self.scopes.len()).rev() {
            for at in (0..self.scopes[scope].walks.len()).rev() {
                let walk = self.scopes[scope].walks[at].clone();
                self.end_walk(walk)?;
            }
            self.end_owned(scope, 0)?;
            for at in (0..self.scopes[scope].vars.len()).rev() {
                let var = self.scopes[scope].vars[at];
                self.b.storage_dead(var)?;
            }
        }
        Ok(())
    }

    // ---- what a panic that leaves the function ends

    /// What unwinding does at a call or a check that is written now: it
    /// ends what the function owns here (PAN-6), in blocks that all
    /// calls with the same owners share.
    pub fn unwind(&mut self) -> Built<UnwindAction> {
        // Nothing is unwound where a panic ends the program.
        if self.quiet || self.shared.aborts {
            return Ok(UnwindAction::Continue);
        }
        if self.ending > 0 {
            return Ok(UnwindAction::Terminate);
        }
        // A place that gave its value up and is not empty yet must not
        // be ended: nothing of this function is, which loses what it
        // owns and frees nothing twice.
        let head = match self.moved.is_empty() {
            true => self.unwind_head()?,
            false => self.frame_base()?,
        };
        Ok(head.map_or(UnwindAction::Continue, UnwindAction::Cleanup))
    }

    /// A block that unwinding runs: what `fill` writes, and then the
    /// block `below`, or the unwinding goes on.
    fn cleanup_block(
        &mut self,
        below: Option<BlockId>,
        fill: impl FnOnce(&mut Self) -> Built,
    ) -> Built<BlockId> {
        let back = self.b.current();
        self.b.set_unwinding(true);
        let block = self.b.new_block();
        self.b.switch_to(block);
        self.ending += 1;
        let filled = fill(self);
        self.ending -= 1;
        filled?;
        match below {
            Some(below) => self.b.goto(below)?,
            None => self.b.resume()?,
        }
        self.b.set_unwinding(false);
        if let Some(back) = back {
            self.b.switch_to(back);
        }
        self.memo.steps += 1;
        Ok(block)
    }

    /// For a function with a frame, the last block of what a panic
    /// that leaves a call runs: the runtime is asked whether a deferred
    /// call of this function stopped the panic. If not, the results end
    /// too; if so, the function goes on and gives them to its caller.
    fn frame_base(&mut self) -> Built<Option<BlockId>> {
        let Some(node) = self.frame.and_then(|frame| frame.node) else {
            return Ok(None);
        };
        if self.frame_block.is_none() {
            let results = self.results_owned.clone();
            let block = self.cleanup_block(None, |this| {
                let bytes = this.bytes_ty();
                let node = this.record(Place::local(node))?;
                let bool = TypeInterner::BOOL;
                let goes_on = this.runtime("_gopp_defer_left", &[bytes], bool, vec![node])?;
                let [ends, done] = [(); 2].map(|()| this.b.new_block());
                this.b.switch(goes_on, vec![(0, ends)], done)?;
                this.b.switch_to(ends);
                for at in results.rev() {
                    let (place, ends) = this.scopes[0].owned[at].clone();
                    this.end(place, ends)?;
                }
                for (var, ends) in this.hidden.clone() {
                    this.end(Place::local(var), ends)?;
                }
                this.b.goto(done)?;
                this.b.switch_to(done);
                Ok(())
            })?;
            self.frame_block = Some(block);
        }
        Ok(self.frame_block)
    }

    /// The first block that unwinding runs for what the scopes and the
    /// temporaries own now; none if they own nothing.
    fn unwind_head(&mut self) -> Built<Option<BlockId>> {
        if let Some((owners, head)) = self.unwind_head
            && owners == self.owners
        {
            return Ok(head);
        }
        // The scopes around the innermost one do not change while it
        // is open: the first of them that has all its blocks is where
        // the new blocks start.
        let mut first = self.scopes.len();
        while first > 0 && !self.scopes[first - 1].unwind.whole {
            first -= 1;
            self.memo.steps += 1;
        }
        let mut head = match first {
            0 => self.frame_base()?,
            _ => Self::chain_head(&self.scopes[first - 1].unwind),
        };
        for scope in first..self.scopes.len() {
            head = self.scope_blocks(scope, head)?;
        }
        // The temporaries stand on the scopes.
        if self.temp_base != head {
            self.temp_heads.clear();
            self.temp_base = head;
        }
        let mut top = self.temp_heads.last().copied().unwrap_or(head);
        for at in self.temp_heads.len()..self.temps.len() {
            let (place, ends) = self.temps[at].clone();
            let owner = temp_key(&place).and_then(|key| self.owning.get(&key).copied());
            if owner == Some(at) {
                top = Some(self.cleanup_block(top, |this| {
                    // The address of a temporary that a call wrote is
                    // not taken: its value ends in a slot of its own.
                    let slot = this.slot_of(Rvalue::Use(Operand::Copy(place)))?;
                    this.end(slot, ends)
                })?);
            }
            self.temp_heads.push(top);
        }
        self.unwind_head = Some((self.owners, top));
        Ok(top)
    }

    fn chain_head(chain: &Chain) -> Option<BlockId> {
        let last = chain.walks.last().or(chain.owned.last());
        last.copied().unwrap_or(chain.base.flatten())
    }

    /// The blocks for what the scope owns, on top of the block `below`:
    /// the first of them to run.
    fn scope_blocks(&mut self, scope: usize, below: Option<BlockId>) -> Built<Option<BlockId>> {
        let chain = &mut self.scopes[scope].unwind;
        if chain.base != Some(below) {
            *chain = Chain {
                base: Some(below),
                ..Chain::default()
            };
        }
        let owned = self.scopes[scope].owned.len();
        if self.scopes[scope].unwind.walks_on != owned {
            self.scopes[scope].unwind.walks.clear();
        }
        // The frame of the function ends the results itself.
        let results = match (scope, self.frame) {
            (0, Some(_)) => self.results_owned.clone(),
            _ => 0..0,
        };
        let mut head = self.scopes[scope].unwind.owned.last().copied();
        let mut head = head.take().unwrap_or(below);
        for at in self.scopes[scope].unwind.owned.len()..owned {
            if !results.contains(&at) {
                let (place, ends) = self.scopes[scope].owned[at].clone();
                head = Some(self.cleanup_block(head, |this| this.end(place, ends))?);
            }
            self.scopes[scope].unwind.owned.push(head);
        }
        self.scopes[scope].unwind.walks_on = owned;
        let mut head = (self.scopes[scope].unwind.walks.last().copied()).unwrap_or(head);
        for at in self.scopes[scope].unwind.walks.len()..self.scopes[scope].walks.len() {
            let walk = self.scopes[scope].walks[at].clone();
            head = Some(self.cleanup_block(head, |this| this.end_walk(walk))?);
            self.scopes[scope].unwind.walks.push(head);
        }
        self.scopes[scope].unwind.whole = true;
        Ok(head)
    }

    /// The types of the results of the function that is lowered, as
    /// the checker has them.
    fn result_types(&self) -> Vec<CheckedTypeId> {
        let types = self.shared.types;
        let sig = match self.owner {
            Some((job, None)) => Some(job.sig),
            Some((_, Some(literal))) => self.body.types.get(&literal).map(|typed| typed.ty),
            None => None,
        };
        match sig.map(|sig| types.get(types.underlying(sig))).as_deref() {
            Some(gopp_sema::types::Type::Func { results, .. }) => results.to_vec(),
            _ => Vec::new(),
        }
    }

    /// The walk of a map is over.
    fn end_walk(&mut self, walk: Operand) -> Built {
        let params = [self.bytes_ty()];
        self.ending += 1;
        let ended = self.runtime_call("_gopp_map_iter_end", &params, vec![walk]);
        self.ending -= 1;
        ended
    }

    /// What the places of the scope own from the place `from` on is
    /// freed, the last first.
    fn end_owned(&mut self, scope: usize, from: usize) -> Built {
        for at in (from..self.scopes[scope].owned.len()).rev() {
            let (place, ends) = self.scopes[scope].owned[at].clone();
            self.end(place, ends)?;
        }
        Ok(())
    }

    /// The variable of the local owns its value, if its type holds what
    /// is freed: the innermost scope ends it.
    fn owns(&mut self, local: CheckedLocalId, var: LocalId) -> Built {
        let ty = self.b.place_ty(&Place::local(var));
        if self.flat(ty) {
            return Ok(());
        }
        let declared = &self.body.locals[local.0 as usize];
        let (checked, span) = (declared.ty, declared.span);
        if let Some(ends) = self.ends_of(Some(checked), span)? {
            self.hold(Place::local(var), ends);
        }
        Ok(())
    }

    /// How the variable of the function ends its value, if it owns one
    /// that is freed.
    pub fn var_ends(&self, var: LocalId) -> Option<Ends> {
        let place = Place::local(var);
        let owned = self.scopes.iter().flat_map(|scope| &scope.owned);
        owned
            .filter(|(of, _)| *of == place)
            .map(|&(_, ends)| ends)
            .next()
    }

    /// Whether the function has `defer` statements: their calls may
    /// change its results.
    pub fn defers_calls(&self) -> bool {
        self.frame.is_some()
    }

    /// How a place of the type ends its value; none, with a report, for
    /// a type that has no drop glue yet.
    pub fn ends_of(
        &mut self,
        checked: Option<CheckedTypeId>,
        span: SourceSpan,
    ) -> Built<Option<Ends>> {
        if let Some(checked) = checked
            && let Ok(lowered) = self.memo.lower_type(self.shared, self.b.types(), checked)
            && matches!(self.kind(lowered), Type::String)
        {
            return Ok(Some(Ends::Text));
        }
        Ok(self.drop_glue(checked, span)?.map(Ends::Glue))
    }

    /// The drop glue of the type, which the runtime is given for the
    /// elements of a slice; none, with a report, for a type that has
    /// none yet.
    pub fn drop_glue(
        &mut self,
        checked: Option<CheckedTypeId>,
        span: SourceSpan,
    ) -> Built<Option<FuncId>> {
        let glue = match checked {
            Some(checked) => self.glue(Glue::Drop, checked)?,
            None => Err("the end of a value whose type is not known"),
        };
        match glue {
            Ok(glue) => Ok(Some(glue)),
            Err(what) => {
                self.unsupported(span, what);
                Ok(None)
            }
        }
    }

    /// The temporary, or the part of a temporary that a call gave,
    /// owns its value from here on.
    pub fn own_temp(&mut self, place: Place, glue: Ends) {
        if let Some(key) = temp_key(&place)
            && !self.owning.contains_key(&key)
        {
            self.owning.insert(key, self.temps.len());
            self.temps.push((place, glue));
            self.owners += 1;
        }
    }

    pub fn owns_temp(&self, place: &Place) -> bool {
        temp_key(place).is_some_and(|key| self.owning.contains_key(&key))
    }

    /// The temporary gives its value up: whether it owned one.
    pub fn take_temp(&mut self, place: &Place) -> bool {
        let Some(at) = temp_key(place).and_then(|key| self.owning.remove(&key)) else {
            return false;
        };
        self.temps_changed(at);
        true
    }

    /// The temporary gives its value to another slot, which owns it
    /// from here on and ends it the same way.
    pub fn rehome_temp(&mut self, from: &Place, to: &Place) {
        let (Some(old), Some(new)) = (temp_key(from), temp_key(to)) else {
            return;
        };
        let Some(at) = self.owning.remove(&old) else {
            return;
        };
        if let Some(entry) = self.temps.get_mut(at) {
            entry.0 = to.clone();
        }
        self.owning.insert(new, at);
        self.temps_changed(at);
    }

    /// The temporaries from the one with the number `at` on own
    /// something else than before.
    fn temps_changed(&mut self, at: usize) {
        self.temp_heads.truncate(at);
        self.owners += 1;
    }

    /// The places that gave their values up are empty from here on:
    /// before anything is stored, called or left.
    pub fn empty_moved(&mut self) -> Built {
        for (place, ty) in std::mem::take(&mut self.moved) {
            let zero = Rvalue::Use(self.zero(ty));
            self.b.assign(place, zero)?;
        }
        Ok(())
    }

    /// The temporaries from `mark` on that still own their values are
    /// freed, the last first: nothing took the values.
    pub fn drop_temps(&mut self, mark: usize) -> Built {
        self.empty_moved()?;
        while self.temps.len() > mark {
            let Some((place, glue)) = self.temps.pop() else {
                break;
            };
            if !self.took_temp(&place) {
                continue;
            }
            // The address of a temporary that a call wrote is not
            // taken: its value ends in a slot of its own.
            let slot = self.slot_of(Rvalue::Use(Operand::Copy(place)))?;
            self.end(slot, glue)?;
        }
        Ok(())
    }

    /// What `aside` writes is a way that the function may take and
    /// what follows does not come after: the temporaries own afterwards
    /// what they owned before, whatever that way ended.
    pub fn aside(&mut self, way: impl FnOnce(&mut Self) -> Built) -> Built {
        let before = (self.temps.clone(), self.owning.clone(), self.moved.clone());
        let went = way(self);
        (self.temps, self.owning, self.moved) = before;
        self.temp_heads.clear();
        self.owners += 1;
        went
    }

    /// The temporary with the highest number was taken off the list:
    /// whether it owned its value, which it gives up.
    fn took_temp(&mut self, place: &Place) -> bool {
        let at = self.temps.len();
        self.temps_changed(at);
        let Some(key) = temp_key(place) else {
            return false;
        };
        match self.owning.get(&key) {
            Some(&owner) if owner == at => self.owning.remove(&key).is_some(),
            _ => false,
        }
    }

    /// The innermost scope ends the value in the place where it is
    /// left.
    pub fn hold(&mut self, place: Place, ends: Ends) {
        if let Some(scope) = self.scopes.last_mut() {
            scope.owned.push((place, ends));
            scope.unwind.whole = false;
            self.owners += 1;
        }
    }

    /// The temporaries from `mark` on live as long as the innermost
    /// scope: what a loop walks.
    pub fn keep_temps(&mut self, mark: usize) -> Built {
        let mark = mark.min(self.temps.len());
        let kept: Vec<_> = self.temps.drain(mark..).collect();
        self.temps_changed(mark);
        for (at, (place, glue)) in kept.into_iter().enumerate() {
            let key = temp_key(&place);
            let owner = key.and_then(|key| self.owning.get(&key).copied());
            if owner != Some(mark + at) {
                continue;
            }
            if let Some(key) = key {
                self.owning.remove(&key);
            }
            // In a slot whose address is taken where the scope ends.
            let slot = self.slot_of(Rvalue::Use(Operand::Copy(place)))?;
            self.hold(slot, glue);
        }
        Ok(())
    }

    /// The innermost scope walks a map from here on.
    pub fn walked(&mut self, walk: Operand) {
        if let Some(scope) = self.scopes.last_mut() {
            scope.walks.push(walk);
            scope.unwind.whole = false;
            self.owners += 1;
        }
    }

    /// A new variable of the innermost scope for the local that the
    /// checker knows.
    pub fn declare(&mut self, local: CheckedLocalId) -> Built<LocalId> {
        let declared = &self.body.locals[local.0 as usize];
        let ty = self.lower(declared.ty, declared.span);
        if self.facts.celled.contains(&local) {
            return self.declare_cell(local, ty);
        }
        let var = self.declare_as(local, ty)?;
        self.owns(local, var)?;
        Ok(var)
    }

    /// Whether the value of the function literal cannot outlive the
    /// variables that it uses: its environment is in the frame.
    pub fn framed(&self, literal: ExprId) -> bool {
        self.facts.framed.contains(&literal)
    }

    /// The first function literal that is not framed and uses the
    /// local.
    pub fn later(&self, local: CheckedLocalId) -> Option<ExprId> {
        self.facts.later.get(&local).copied()
    }

    /// Whether a function literal that is not framed uses the local,
    /// which then lives in a cell.
    pub fn celled(&self, local: CheckedLocalId) -> bool {
        self.facts.celled.contains(&local)
    }

    /// Whether a `defer` statement borrows the local, a variable of a
    /// block.
    pub fn extended(&self, local: CheckedLocalId) -> bool {
        self.facts.extended.contains(&local)
    }

    /// The type of the cell of a variable of the type: the count of
    /// what shares the cell, then the variable.
    pub fn cell_type(&mut self, of: TypeId) -> TypeId {
        let word = self.b.types().int(64, false);
        self.b.types().tuple(&[word, of])
    }

    /// A new variable of the type `ty` for a local that a function
    /// literal uses (CLO-2): it lives in a new cell, which the innermost
    /// scope lets go where it is left and every function value that
    /// uses the variable shares. The variable is another name of what
    /// the cell holds, which is all zero.
    fn declare_cell(&mut self, local: CheckedLocalId, ty: TypeId) -> Built<LocalId> {
        let declared = &self.body.locals[local.0 as usize];
        let (checked, span) = (declared.ty, declared.span);
        let drop = match self.flat(ty) {
            true => None,
            false => self.drop_glue(Some(checked), span)?,
        };
        let cell = self.new_cell(local, ty, drop)?;
        self.hold(Place::local(cell), Ends::Cell(drop));
        if let Some(scope) = self.scopes.last_mut() {
            scope.cells.push(local);
        }
        self.name_cell(local, cell, ty)
    }

    /// A new cell for a variable of the type `ty`, all zero, in a slot
    /// of its own. A variable of a block that a `defer` statement may
    /// borrow has more in its cell, after the variable: what the list
    /// of the borrowed variables keeps of it, with its number among
    /// those that were declared.
    pub fn new_cell(
        &mut self,
        local: CheckedLocalId,
        ty: TypeId,
        drop: Option<FuncId>,
    ) -> Built<LocalId> {
        let plain = self.cell_type(ty);
        let word = self.b.types().int(64, false);
        let bytes = self.bytes_ty();
        let extended = self.facts.extended.contains(&local) && self.frame.is_some();
        let whole = match extended {
            true => {
                let glue = self.glue_sig(Glue::Drop);
                let types = self.b.types();
                let kept = types.tuple(&[bytes, word, bytes, glue]);
                types.tuple(&[word, ty, kept])
            }
            false => plain,
        };
        let layout = self.b.types().layout(whole, self.shared.layout);
        let size = self
            .b
            .const_int(word, layout.map_or(0, |layout| layout.size))?;
        let made = self.runtime("_gopp_cell_new", &[word], bytes, vec![size])?;
        let to = self.b.types().ptr(plain);
        let cell = Rvalue::Cast(crate::CastKind::PtrToPtr, made.clone(), to);
        let cell = self.b.assign_temp(cell)?;
        if extended {
            let count = match self.declared {
                Some(count) => count,
                None => *self.declared.insert(self.b.temp(word)),
            };
            let count = Place::local(count);
            let one = self.b.const_int(word, 1)?;
            let next = Rvalue::Binary(crate::BinOp::Add, Operand::Copy(count.clone()), one);
            self.b.assign(count.clone(), next)?;
            let to = self.b.types().ptr(whole);
            let all = Rvalue::Cast(crate::CastKind::PtrToPtr, made, to);
            let all = Place::local(self.b.assign_temp(all)?);
            let kept = self.b.deref(&all)?;
            let kept = self.b.field(&kept, 2)?;
            let number = self.b.field(&kept, 1)?;
            self.b.assign(number, Rvalue::Use(Operand::Copy(count)))?;
            self.extendable.insert(local, (cell, whole, drop));
        }
        Ok(cell)
    }

    /// A `defer` statement that runs borrows the variable: if it is a
    /// variable of a block that this function declares, it lives until
    /// the function is left, after the deferred calls (PAN-3, PAN-4).
    pub fn extend(&mut self, local: CheckedLocalId) -> Built {
        let Some(&(cell, whole, drop)) = self.extendable.get(&local) else {
            return Ok(());
        };
        let bytes = self.bytes_ty();
        // A variable of a block is declared in a block of the function,
        // which has a list.
        let Some(list) = self.region else {
            return Ok(());
        };
        let list = self.record(Place::local(list))?;
        let held = Operand::Copy(Place::local(cell));
        let to = self.b.types().ptr(whole);
        let all = Rvalue::Cast(crate::CastKind::PtrToPtr, held.clone(), to);
        let all = Place::local(self.b.assign_temp(all)?);
        let kept = self.b.deref(&all)?;
        let kept = self.b.field(&kept, 2)?;
        let kept = self.record(kept)?;
        let held = Rvalue::Cast(crate::CastKind::PtrToPtr, held, bytes);
        let held = Operand::Copy(Place::local(self.b.assign_temp(held)?));
        let glue = self.glue_sig(Glue::Drop);
        let drop = match drop {
            Some(drop) => self.glue_value(Glue::Drop, drop),
            None => self.zero(glue),
        };
        let params = [bytes, bytes, bytes, glue];
        self.runtime_call("_gopp_extend", &params, vec![list, kept, held, drop])
    }

    /// Whether the variable lives in a cell here: its own, or one of a
    /// function around this one.
    pub fn in_cell(&self, local: CheckedLocalId) -> bool {
        self.cells.contains_key(&local)
    }

    /// The variable of the local as another name of what the cell in
    /// the slot holds.
    pub fn name_cell(
        &mut self,
        local: CheckedLocalId,
        cell: LocalId,
        ty: TypeId,
    ) -> Built<LocalId> {
        let var = self.declare_alias(local, ty)?;
        let inside = self.b.deref(&Place::local(cell))?;
        let inside = self.b.field(&inside, 1)?;
        let address = Rvalue::AddrOf(inside, crate::Mutability::Mut);
        self.b.assign(Place::local(var), address)?;
        self.cells.insert(local, cell);
        self.cell_vars.insert(var, local);
        Ok(var)
    }

    /// How the value of a variable that lives in a cell ends, where
    /// the variable gets another value: its cell ends it only with
    /// itself. None for a variable in no cell, and for a value that
    /// owns nothing.
    pub fn cell_ends(&mut self, var: LocalId, span: SourceSpan) -> Built<Option<Ends>> {
        let Some(&local) = self.cell_vars.get(&var) else {
            return Ok(None);
        };
        let checked = self.body.locals[local.0 as usize].ty;
        let ty = self.lower(checked, span);
        match self.flat(ty) {
            true => Ok(None),
            false => self.ends_of(Some(checked), span),
        }
    }

    /// The slot of the variable of the local, if it is declared.
    pub fn vars_of(&self, local: CheckedLocalId) -> Option<&LocalId> {
        self.vars.get(&local)
    }

    /// The variables of the innermost scope that live in cells.
    pub fn scope_cells(&self) -> Vec<CheckedLocalId> {
        self.scopes
            .last()
            .map_or(Vec::new(), |scope| scope.cells.clone())
    }

    /// The place of the variable in the slot: what the slot holds, or
    /// what it is another name of.
    pub fn var_place(&mut self, var: LocalId) -> Built<Place> {
        match self.aliases.contains(&var) {
            true => self.b.deref(&Place::local(var)),
            false => Ok(Place::local(var)),
        }
    }

    /// A new variable that does not own its value: it is read while the
    /// owner of the value lives.
    pub fn declare_view(&mut self, local: CheckedLocalId) -> Built<LocalId> {
        let declared = &self.body.locals[local.0 as usize];
        let ty = self.lower(declared.ty, declared.span);
        let var = self.declare_as(local, ty)?;
        self.borrowed.insert(var);
        Ok(var)
    }

    /// The variable of the local, if it is declared and owns its
    /// value: no other name of an element, and no element as it is.
    pub fn owning_var(&self, local: CheckedLocalId) -> Option<LocalId> {
        let var = *self.vars.get(&local)?;
        let own = !self.aliases.contains(&var)
            && !self.borrowed.contains(&var)
            && !self.lent_params.contains(&var);
        own.then_some(var)
    }

    /// Whether the local is a variable of a block inside the function:
    /// it ends before the function is left.
    pub fn of_block(&self, local: CheckedLocalId) -> bool {
        let var = self.vars.get(&local);
        let inner = self.scopes.iter().skip(1);
        var.is_some_and(|var| inner.clone().any(|scope| scope.vars.contains(var)))
    }

    /// Whether the local has a variable of its own, which is no other
    /// name of an element.
    pub fn plain_var(&self, local: CheckedLocalId) -> bool {
        let var = self.vars.get(&local);
        var.is_some_and(|var| !self.aliases.contains(var))
    }

    /// Whether the local is a parameter that the function calls
    /// methods through: its value is the function's to let be changed.
    pub fn worked_param(&self, local: CheckedLocalId) -> bool {
        let var = self.vars.get(&local);
        var.is_some_and(|var| self.worked_params.contains(var))
    }

    /// Whether the variable of the local reads a value that another
    /// owns: an element as it is, an argument that was lent.
    pub fn reads_only(&self, local: CheckedLocalId) -> bool {
        let var = self.vars.get(&local);
        var.is_some_and(|var| self.borrowed.contains(var) || self.lent_params.contains(var))
    }

    /// Whether the value of the variable cannot change while a call
    /// runs: nothing but this function reaches the variable, and it is
    /// no other name of an element, which its collection may change.
    pub fn quiet_var(&self, local: CheckedLocalId) -> bool {
        let Some(var) = self.vars.get(&local) else {
            return false;
        };
        self.facts.quiet(local) && !self.aliases.contains(var) && !self.borrowed.contains(var)
    }

    /// A new variable for the local that is another name of a place of
    /// the type `of`: it holds the address of the place.
    pub fn declare_alias(&mut self, local: CheckedLocalId, of: TypeId) -> Built<LocalId> {
        let ty = self.b.types().ptr(of);
        let var = self.declare_as(local, ty)?;
        self.aliases.insert(var);
        Ok(var)
    }

    fn declare_as(&mut self, local: CheckedLocalId, ty: TypeId) -> Built<LocalId> {
        let declared = &self.body.locals[local.0 as usize];
        // The body of the function is the scope of the function; any
        // other block gets a scope with its first variable.
        let loc = self.loc(declared.span);
        if let [_, .., scope] = &mut self.scopes[..]
            && !std::mem::replace(&mut scope.debug, true)
        {
            self.b.push_scope(ScopeKind::Block, loc);
        }
        let name = self.memo.name(self.shared, declared.name);
        let var = self.b.var(name, ty, true);
        self.b.storage_live(var)?;
        if let Some(scope) = self.scopes.last_mut() {
            scope.vars.push(var);
        }
        self.vars.insert(local, var);
        Ok(var)
    }

    /// The variable of the local; one whose declaration was not
    /// lowered is declared where it is first used.
    pub fn var(&mut self, local: CheckedLocalId) -> Built<LocalId> {
        match self.vars.get(&local) {
            Some(&var) => Ok(var),
            None => {
                // The body of a loop over a function has every variable
                // of the function with the loop that it names.
                let declared = &self.body.locals[local.0 as usize];
                if self
                    .ranged
                    .as_ref()
                    .is_some_and(|ranged| ranged.outside(declared.span))
                {
                    let what = "a variable that the body of a range over a function does not name";
                    self.unsupported(declared.span, what);
                }
                let var = self.declare(local)?;
                let place = self.var_place(var)?;
                let zero = self.zero(self.b.place_ty(&place));
                self.b.assign(place, Rvalue::Use(zero))?;
                Ok(var)
            }
        }
    }

    // ---- labels

    /// Makes the label known, in the scope that is innermost now.
    pub fn label(&mut self, name: Spur) -> BlockId {
        if let Some(label) = self.labels.get(&name) {
            return label.block;
        }
        let label = Label {
            block: self.b.new_block(),
            depth: self.scopes.len(),
            live: None,
        };
        let block = label.block;
        self.labels.insert(name, label);
        block
    }

    /// The label is here: the variables of its scope that are declared
    /// later are not alive at it.
    pub fn label_met(&mut self, name: Spur) {
        if let Some(label) = self.labels.get_mut(&name) {
            let scope = self.scopes.get(label.depth.wrapping_sub(1));
            label.live = Some(scope.map_or((0, 0), |scope| (scope.vars.len(), scope.owned.len())));
        }
    }

    /// Leaves the scopes between here and the label, and the variables
    /// of its own scope that a jump back passes.
    pub fn exit_to_label(&mut self, name: Spur) -> Built<Option<BlockId>> {
        let Some(label) = self.labels.get(&name) else {
            return Ok(None);
        };
        let (block, depth, live) = (label.block, label.depth, label.live);
        self.exit_scopes(depth)?;
        if let (Some((live, owned)), Some(scope)) = (live, depth.checked_sub(1)) {
            // The variables that `defer` statements borrowed stay.
            for at in (owned..self.scopes[scope].owned.len()).rev() {
                let (place, ends) = self.scopes[scope].owned[at].clone();
                if !matches!(ends, Ends::Extended) {
                    self.end(place, ends)?;
                }
            }
            for at in (live..self.scopes[scope].vars.len()).rev() {
                let var = self.scopes[scope].vars[at];
                self.b.storage_dead(var)?;
            }
        }
        Ok(Some(block))
    }

    /// What follows cannot be reached from what was just written; it
    /// goes into a block of its own, which is dropped if nothing jumps
    /// into it.
    pub fn dead(&mut self) {
        let block = self.b.new_block();
        self.b.switch_to(block);
    }

    // ---- what the function needs of its module

    /// The function of the symbol, declared in the module of the run if
    /// it is not there yet.
    pub fn function_named(&mut self, symbol: &str, sig: TypeId) -> Built<FuncId> {
        if let Some(&known) = self.memo.funcs.get(symbol) {
            return Ok(known);
        }
        let module = self.b.module();
        let declared =
            FuncBuilder::new(module, symbol, sig, Linkage::External, Span::default())?.finish()?;
        self.memo.funcs.insert(symbol.to_owned(), declared);
        Ok(declared)
    }

    pub fn effects_known(&self, expr: ExprId) -> Option<bool> {
        self.effects.get(&expr).copied()
    }

    pub fn set_effects(&mut self, expr: ExprId, effects: bool) {
        self.effects.insert(expr, effects);
    }
}
