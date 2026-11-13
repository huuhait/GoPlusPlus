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
}

/// A `defer` statement of the function (PAN-3).
pub(super) struct Deferred {
    pub stmt: StmtId,
    /// Whether the statement ran: its call is made when the function is
    /// left.
    pub flag: LocalId,
    /// The statement is in a loop, where it runs more than once.
    pub in_loop: bool,
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
}

/// A call that waits for its function to be left.
#[derive(Clone)]
pub(super) struct Pending {
    pub what: Waits,
    /// The receiver and the arguments, each in a slot of the function
    /// that owns it, with how it ends if the call does not take it.
    pub slots: Vec<(LocalId, Option<Ends>)>,
    /// How what the call gives ends: nothing takes it.
    pub result: Option<Ends>,
    pub span: SourceSpan,
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
    /// Which file of the package.
    file: u32,
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
    owning: FxHashSet<(LocalId, u32)>,
    /// The variables that do not own their values: a plain range
    /// variable of a Go++ file is the element as it is.
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
    /// For a function with `defer` statements: the block that makes
    /// their calls for every way out, the slot that says which way it
    /// was, and where each way goes on.
    leave: Option<(BlockId, LocalId, Vec<BlockId>)>,
    /// The temporaries that a way out reads after that block. Each
    /// has its value on its own way; the first block gives all of them
    /// one, so that every way has one.
    late: FxHashSet<LocalId>,
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
    if let ExprKind::FuncType { fails: true, .. } = tree.exprs[*ty].kind {
        let message = "not supported yet: a function that can fail".to_owned();
        let diagnostic = Diagnostic::new(Code::GirUnsupported, name.span, message);
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
    let built = FuncBuilder::new(module, &job.symbol, sig, Linkage::External, loc).and_then(|b| {
        let body = &shared.package.files[job.file as usize].bodies[job.at];
        let mut cx = FuncCx::new(shared, &mut *memo, b, job.file, (body, &job.facts));
        cx.function(receiver.as_ref(), *ty, block, 0)?;
        cx.b.drop_unreached();
        cx.b.finish()
    });
    if let Err(error) = built {
        let message = format!("{} could not be lowered: {error}", job.symbol);
        let diagnostic = Diagnostic::new(Code::GirVerify, name.span, message);
        memo.diagnostics.push(diagnostic);
    }
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
    let Some((symbol, _)) = memo.literal(shared, job.file, literal) else {
        return;
    };
    let report = |memo: &mut Memo<'a>, code: Code, message: String| {
        memo.diagnostics
            .push(Diagnostic::new(code, node.span, message));
    };
    if let ExprKind::FuncType { fails: true, .. } = tree.exprs[*ty].kind {
        let message = "not supported yet: a function that can fail".to_owned();
        return report(memo, Code::GirUnsupported, message);
    }
    // The type of the code is that of the first word of the value.
    let body = &shared.package.files[job.file as usize].bodies[job.at];
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
        cx.function(None, *ty, block, 1)?;
        cx.b.drop_unreached();
        cx.b.finish()
    });
    if let Err(error) = built {
        let message = format!("{symbol} could not be lowered: {error}");
        report(memo, Code::GirVerify, message);
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
) {
    let value = memo.lower_type(shared, &mut module.types, ty);
    let sig = value.ok().and_then(|value| match module.types.get(value) {
        Type::Tuple(parts) => parts.first().copied(),
        _ => None,
    });
    let Some(sig) = sig else {
        return not_made(shared, memo, symbol, Ok(Err("a function type")));
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
        let made = cx.value_body(of, ty)?;
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
            owning: FxHashSet::default(),
            borrowed: FxHashSet::default(),
            handed: FxHashSet::default(),
            defers: Vec::new(),
            jumps: false,
            leave: None,
            late: FxHashSet::default(),
            moved: Vec::new(),
        }
    }
}

impl FuncCx<'_, '_> {
    /// The body of a function whose first parameter of the source has
    /// the number `first`: an environment comes before the parameters
    /// of a function literal.
    fn function(
        &mut self,
        receiver: Option<&Param>,
        ty: ExprId,
        block: &Block,
        first: u32,
    ) -> Built {
        let ExprKind::FuncType {
            params, results, ..
        } = &self.tree.exprs[ty].kind
        else {
            return Ok(());
        };
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
        let whole = self.b.place_ty(&Place::local(self.b.result()));
        let parts = match self.kind(whole) {
            Type::Tuple(parts) if results.len() > 1 => parts.to_vec(),
            _ if results.is_empty() => Vec::new(),
            _ => vec![whole],
        };
        let mut next = first + (usize::from(receiver.is_some()) + params.len()) as u32;
        for part in parts {
            let view = super::types::is_view(self.b.types(), part).then(|| {
                next += 1;
                self.b.param(next - 1)
            });
            self.views.push(view);
        }
        // A function with `defer` statements starts by saying that
        // none of them ran. What they keep is known when the body is
        // lowered, so the start is written last.
        self.find_defers(&block.stmts, false);
        // The cells of the parameters and of the results are made
        // before that start, which is a block of its own.
        let made: FxHashSet<LocalId> = self.cells.values().copied().collect();
        let start = match self.defers.is_empty() {
            true => None,
            false => {
                // The first block itself, unless a cell was made in it.
                let [body, leave] = [(); 2].map(|()| self.b.new_block());
                let first = match self.b.current() {
                    Some(current) if current == entry => entry,
                    _ => {
                        let first = self.b.new_block();
                        self.b.goto(first)?;
                        first
                    }
                };
                let word = self.b.types().int(64, false);
                self.leave = Some((leave, self.b.temp(word), Vec::new()));
                self.b.switch_to(body);
                Some((first, body, self.b.loc()))
            }
        };
        self.stmts(&block.stmts)?;
        // The end of the body: a function with results never comes
        // here, as the checker saw.
        self.at_end(block.span);
        match results.is_empty() {
            true => {
                self.run_defers()?;
                self.exit_scopes(0)?;
                self.b.ret()?;
            }
            false => self.b.unreachable()?,
        }
        let Some((entry, body, loc)) = start else {
            return Ok(());
        };
        // The calls are made in one place for every way out, which
        // goes on where it came from.
        let way_out = self.leave.as_ref().map(|&(_, which, _)| which);
        if let Some((leave, which, ways)) = self.leave.take() {
            self.b.switch_to(leave);
            self.deferred_calls()?;
            let which = Operand::Copy(Place::local(which));
            match ways.split_last() {
                Some((&last, rest)) => {
                    let cases = rest.iter().enumerate();
                    let cases = cases.map(|(at, &way)| (at as u64, way)).collect();
                    self.b.switch(which, cases, last)?;
                }
                None => self.b.unreachable()?,
            }
        }
        self.b.switch_to(entry);
        self.b.set_loc(loc);
        // Every slot has a value from the start, on every way.
        let mut early: Vec<LocalId> = way_out.into_iter().collect();
        let late = self.late.drain().filter(|local| !made.contains(local));
        let mut late: Vec<LocalId> = late.collect();
        late.sort_by_key(|local| local.into_raw().into_u32());
        early.extend(late);
        for slot in early {
            let none = self.zero(self.b.place_ty(&Place::local(slot)));
            self.b.assign(Place::local(slot), Rvalue::Use(none))?;
        }
        for at in 0..self.defers.len() {
            let no = self.b.const_bool(false);
            let flag = Place::local(self.defers[at].flag);
            self.b.assign(flag, Rvalue::Use(no))?;
            let slots = self.defers[at].call.as_ref().map(|call| call.slots.clone());
            for (slot, _) in slots.unwrap_or_default() {
                let empty = self.zero(self.b.place_ty(&Place::local(slot)));
                self.b.assign(Place::local(slot), Rvalue::Use(empty))?;
            }
        }
        self.b.goto(body)
    }

    /// The `defer` statements of the statements and of the blocks in
    /// them, in the order of the source; `in_loop` for those that a
    /// loop has.
    fn find_defers(&mut self, stmts: &[StmtId], in_loop: bool) {
        let tree = self.tree;
        for &stmt in stmts {
            self.memo.steps += 1;
            match &tree.stmts[stmt].kind {
                StmtKind::Defer(_) => {
                    let flag = self.b.temp(TypeInterner::BOOL);
                    self.defers.push(Deferred {
                        stmt,
                        flag,
                        in_loop,
                        call: None,
                    });
                }
                StmtKind::Labeled { stmt, .. } => {
                    self.jumps = true;
                    self.find_defers(&[*stmt], in_loop);
                }
                StmtKind::Branch {
                    kind: BranchKind::Goto,
                    ..
                } => self.jumps = true,
                StmtKind::Block(block) => deeper(|| self.find_defers(&block.stmts, in_loop)),
                StmtKind::If {
                    then, otherwise, ..
                } => {
                    deeper(|| self.find_defers(&then.stmts, in_loop));
                    self.find_defers(otherwise.as_slice(), in_loop);
                }
                StmtKind::For { body, .. } | StmtKind::Range { body, .. } => {
                    deeper(|| self.find_defers(&body.stmts, true));
                }
                StmtKind::Switch { clauses, .. } => {
                    for clause in clauses {
                        deeper(|| self.find_defers(&clause.body, in_loop));
                    }
                }
                StmtKind::Select { clauses } => {
                    for clause in clauses {
                        deeper(|| self.find_defers(&clause.body, in_loop));
                    }
                }
                _ => {}
            }
        }
    }

    /// The function is left: the calls of its `defer` statements are
    /// made first (PAN-4), in the block that all ways out share.
    pub fn run_defers(&mut self) -> Built {
        let Some((leave, which, ways)) = &mut self.leave else {
            return Ok(());
        };
        let (leave, which, way) = (*leave, *which, ways.len());
        // What the scopes end after the calls is read after them.
        let mut read = Vec::new();
        for scope in &self.scopes {
            read.extend(scope.owned.iter().map(|(place, _)| place.local));
            for walk in &scope.walks {
                if let Operand::Copy(place) | Operand::Move(place) = walk {
                    read.push(place.local);
                }
            }
        }
        for local in read {
            self.read_late(local);
        }
        let after = self.b.new_block();
        if let Some((_, _, ways)) = &mut self.leave {
            ways.push(after);
        }
        let word = self.b.types().int(64, false);
        let way = self.b.const_int(word, way as u64)?;
        self.b.assign(Place::local(which), Rvalue::Use(way))?;
        self.b.goto(leave)?;
        self.b.switch_to(after);
        Ok(())
    }

    /// The local is read after the block that makes the deferred calls.
    pub fn read_late(&mut self, local: LocalId) {
        if self.b.local_kind(local) == crate::LocalKind::Temp {
            self.late.insert(local);
        }
    }

    /// The calls of the `defer` statements that ran, the last first. A
    /// statement that is in no loop runs after those before it in the
    /// source, so the order of the source turned around is theirs.
    fn deferred_calls(&mut self) -> Built {
        for at in (0..self.defers.len()).rev() {
            // A statement that was refused keeps nothing.
            let Some(pending) = self.defers[at].call.clone() else {
                continue;
            };
            let ran = Operand::Copy(Place::local(self.defers[at].flag));
            let [call, done, after] = [(); 3].map(|()| self.b.new_block());
            self.b.switch(ran, vec![(0, done)], call)?;
            self.b.switch_to(call);
            let slot = |&(slot, _): &(LocalId, Option<Ends>)| Operand::Copy(Place::local(slot));
            let given: Vec<Operand> = pending.slots.iter().map(slot).collect();
            let around = self.b.loc();
            self.at(pending.span);
            // Whether the call leaves a value to the statement: a
            // parameter that is only read does, and so does `print`.
            let lent: Vec<bool> = match &pending.what {
                Waits::Call(called) => {
                    let callee = Operand::Const(Constant {
                        kind: ConstantKind::Func(called.func),
                        ty: called.sig,
                    });
                    let result = Place::local(self.b.temp(called.ret));
                    let unwind = UnwindAction::Continue;
                    self.b
                        .call(callee, given, result.clone(), Some(after), unwind)?;
                    self.b.switch_to(after);
                    // The address of a temporary that a call wrote is
                    // not taken: its value ends in a slot of its own.
                    if let Some(ends) = pending.result {
                        let slot = self.slot_of(Rvalue::Use(Operand::Copy(result)))?;
                        self.end(slot, ends)?;
                    }
                    let lent = |param| called.lent.get(param) == Some(&true);
                    (0..pending.slots.len()).map(lent).collect()
                }
                Waits::Print(line) => {
                    self.b.goto(after)?;
                    self.b.switch_to(after);
                    let values = given.into_iter().map(|value| (value, pending.span));
                    self.print_values(*line, values.collect())?;
                    vec![true; pending.slots.len()]
                }
            };
            for (&(slot, ends), lent) in pending.slots.iter().zip(lent) {
                if let (true, Some(ends)) = (lent, ends) {
                    self.end(Place::local(slot), ends)?;
                }
            }
            self.b.set_loc(around);
            self.b.goto(done)?;
            self.b.switch_to(done);
        }
        Ok(())
    }

    // ---- places in the source

    /// Which file of the package the function is in.
    pub fn file(&self) -> u32 {
        self.file
    }

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
        if self.reported.insert((span.lo, span.hi)) {
            let message = format!("not supported yet: {what}");
            let diagnostic = Diagnostic::new(Code::GirUnsupported, span, message);
            self.memo.diagnostics.push(diagnostic);
        }
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

    /// The zero of the type.
    pub fn zero(&self, ty: TypeId) -> Operand {
        Operand::Const(Constant {
            kind: ConstantKind::Zero,
            ty,
        })
    }

    // ---- variables and scopes

    pub fn open_scope(&mut self) {
        self.scopes.push(Scope::default());
    }

    /// Ends the innermost scope, which the source ends at the end of
    /// `span`: its variables are dead from here.
    pub fn close_scope(&mut self, span: SourceSpan) -> Built {
        self.at_end(span);
        self.exit_scopes(self.scopes.len() - 1)?;
        if self.scopes.pop().is_some_and(|scope| scope.debug) {
            self.b.pop_scope();
        }
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
                let params = [self.bytes_ty()];
                self.runtime_call("_gopp_map_iter_end", &params, vec![walk])?;
            }
            self.end_owned(scope, 0)?;
            for at in (0..self.scopes[scope].vars.len()).rev() {
                let var = self.scopes[scope].vars[at];
                self.b.storage_dead(var)?;
            }
        }
        Ok(())
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
        if let Some(ends) = self.ends_of(Some(checked), span)?
            && let Some(scope) = self.scopes.last_mut()
        {
            scope.owned.push((Place::local(var), ends));
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
        self.leave.is_some()
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
            && self.owning.insert(key)
        {
            self.temps.push((place, glue));
        }
    }

    pub fn owns_temp(&self, place: &Place) -> bool {
        temp_key(place).is_some_and(|key| self.owning.contains(&key))
    }

    /// The temporary gives its value up: whether it owned one.
    pub fn take_temp(&mut self, place: &Place) -> bool {
        temp_key(place).is_some_and(|key| self.owning.remove(&key))
    }

    /// The places that gave their values up are empty from here on:
    /// before anything is stored, called or left.
    pub fn empty_moved(&mut self) -> Built {
        for (place, ty) in std::mem::take(&mut self.moved) {
            self.b.assign(place, Rvalue::Use(self.zero(ty)))?;
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
            if !self.take_temp(&place) {
                continue;
            }
            // The address of a temporary that a call wrote is not
            // taken: its value ends in a slot of its own.
            let slot = self.slot_of(Rvalue::Use(Operand::Copy(place)))?;
            self.end(slot, glue)?;
        }
        Ok(())
    }

    /// The innermost scope ends the value in the place where it is
    /// left.
    pub fn hold(&mut self, place: Place, ends: Ends) {
        if let Some(scope) = self.scopes.last_mut() {
            scope.owned.push((place, ends));
        }
    }

    /// The temporaries from `mark` on live as long as the innermost
    /// scope: what a loop walks.
    pub fn keep_temps(&mut self, mark: usize) -> Built {
        let kept: Vec<_> = self.temps.drain(mark.min(self.temps.len())..).collect();
        for (place, glue) in kept {
            if !self.take_temp(&place) {
                continue;
            }
            // In a slot whose address is taken where the scope ends.
            let slot = self.slot_of(Rvalue::Use(Operand::Copy(place)))?;
            if let Some(scope) = self.scopes.last_mut() {
                scope.owned.push((slot, glue));
            }
        }
        Ok(())
    }

    /// The innermost scope walks a map from here on.
    pub fn walked(&mut self, walk: Operand) {
        if let Some(scope) = self.scopes.last_mut() {
            scope.walks.push(walk);
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

    /// Whether a function literal uses the local, which then lives in
    /// a cell.
    pub fn celled(&self, local: CheckedLocalId) -> bool {
        self.facts.celled.contains(&local)
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
        let cell_ty = self.cell_type(ty);
        let layout = self.b.types().layout(cell_ty, self.shared.layout);
        let word = self.b.types().int(64, false);
        let size = self
            .b
            .const_int(word, layout.map_or(0, |layout| layout.size))?;
        let bytes = self.bytes_ty();
        let made = self.runtime("_gopp_cell_new", &[word], bytes, vec![size])?;
        let to = self.b.types().ptr(cell_ty);
        let cell = self
            .b
            .assign_temp(Rvalue::Cast(crate::CastKind::PtrToPtr, made, to))?;
        let drop = match self.flat(ty) {
            true => None,
            false => self.drop_glue(Some(checked), span)?,
        };
        self.hold(Place::local(cell), Ends::Cell(drop));
        if let Some(scope) = self.scopes.last_mut() {
            scope.cells.push(local);
        }
        self.name_cell(local, cell, ty)
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
            self.end_owned(scope, owned)?;
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
