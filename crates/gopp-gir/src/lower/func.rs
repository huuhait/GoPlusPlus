//! One function: its parameters, its variables and the scopes they
//! live in.

use gopp_diag::{Code, Diagnostic};
use gopp_sema::types::TypeId as CheckedTypeId;
use gopp_sema::{Body, LocalId as CheckedLocalId};
use gopp_span::{SourceFile, Span as SourceSpan};
use gopp_syntax::ast::{Block, DeclKind, ExprId, ExprKind, File, Param};
use lasso::Spur;
use rustc_hash::{FxHashMap, FxHashSet};

use super::facts::Facts;
use super::glue::{Ends, Glue};
use super::{Job, Memo, Shared};
use crate::{
    BlockId, BuildError, Constant, ConstantKind, FuncBuilder, FuncId, Linkage, LocalId, Module,
    Operand, Place, ProjectionKind, Rvalue, ScopeKind, Span, Type, TypeId, TypeInterner,
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
    /// Whether the block has a debug scope: it gets one with its first
    /// variable.
    debug: bool,
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
    /// The parameters that the function only reads: its caller owns
    /// their values.
    lent_params: FxHashSet<LocalId>,
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
        cx.function(receiver.as_ref(), *ty, block)?;
        cx.b.drop_unreached();
        cx.b.finish()
    });
    if let Err(error) = built {
        let message = format!("{} could not be lowered: {error}", job.symbol);
        let diagnostic = Diagnostic::new(Code::GirVerify, name.span, message);
        memo.diagnostics.push(diagnostic);
    }
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
    // What asked for the glue saw that the type has one: a glue that
    // is not made is a fault of the lowering.
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
            lent_params: FxHashSet::default(),
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
            moved: Vec::new(),
        }
    }
}

impl FuncCx<'_, '_> {
    fn function(&mut self, receiver: Option<&Param>, ty: ExprId, block: &Block) -> Built {
        let ExprKind::FuncType {
            params, results, ..
        } = &self.tree.exprs[ty].kind
        else {
            return Ok(());
        };
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        self.scopes.push(Scope::default());
        for (at, param) in receiver.into_iter().chain(params).enumerate() {
            let declared = param
                .name
                .and_then(|name| self.body.defs.get(&name.span.lo));
            if let (Some(name), Some(&local)) = (param.name, declared) {
                let slot = self.b.param(at as u32);
                self.b
                    .set_name(slot, self.memo.name(self.shared, name.name));
                self.vars.insert(local, slot);
                // A parameter that the function only reads is the
                // caller's; any other is the function's own: the caller
                // gave a copy or the value itself.
                match self.facts.lent.get(at) {
                    Some(true) => drop(self.lent_params.insert(slot)),
                    _ => self.owns(local, slot)?,
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
                    let zero = self.zero(self.b.place_ty(&Place::local(var)));
                    self.b.assign(Place::local(var), Rvalue::Use(zero))?;
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
        let mut next = (usize::from(receiver.is_some()) + params.len()) as u32;
        for part in parts {
            let view = super::types::is_view(self.b.types(), part).then(|| {
                next += 1;
                self.b.param(next - 1)
            });
            self.views.push(view);
        }
        self.stmts(&block.stmts)?;
        // The end of the body: a function with results never comes
        // here, as the checker saw.
        self.at_end(block.span);
        match results.is_empty() {
            true => {
                self.exit_scopes(0)?;
                self.b.ret()
            }
            false => self.b.unreachable(),
        }
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
        let var = self.declare_as(local, ty)?;
        self.owns(local, var)?;
        Ok(var)
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
                let zero = self.zero(self.b.place_ty(&Place::local(var)));
                self.b.assign(Place::local(var), Rvalue::Use(zero))?;
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
