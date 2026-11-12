//! One function: its parameters, its variables and the scopes they
//! live in.

use gopp_diag::{Code, Diagnostic};
use gopp_sema::types::TypeId as CheckedTypeId;
use gopp_sema::{Body, LocalId as CheckedLocalId};
use gopp_span::{SourceFile, Span as SourceSpan};
use gopp_syntax::ast::{Block, DeclKind, ExprId, ExprKind, File, Param};
use lasso::Spur;
use rustc_hash::{FxHashMap, FxHashSet};

use super::{Job, Memo, Shared};
use crate::{
    BlockId, BuildError, Constant, ConstantKind, FuncBuilder, FuncId, Linkage, LocalId, Module,
    Operand, Place, Rvalue, ScopeKind, Span, Type, TypeId, TypeInterner,
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
    /// Whether the block has a debug scope: it gets one with its first
    /// variable.
    debug: bool,
}

/// A label, known when the list of statements that has it is entered.
pub(super) struct Label {
    pub block: BlockId,
    /// How many scopes are open where it is.
    depth: usize,
    /// How many variables its scope has where it is, once it is met.
    live: Option<usize>,
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
}

/// Adds the function of the job to the module, or reports why not.
pub(super) fn lower_function<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    job: &Job,
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
        let mut cx = FuncCx {
            shared,
            memo: &mut *memo,
            b,
            tree,
            source,
            file: job.file,
            body: &shared.package.files[job.file as usize].bodies[job.at],
            vars: FxHashMap::default(),
            scopes: Vec::new(),
            labels: FxHashMap::default(),
            targets: Vec::new(),
            results: Vec::new(),
            label: None,
            effects: FxHashMap::default(),
            reported: FxHashSet::default(),
            reused: None,
        };
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
    /// innermost and the last declared first. The scopes stay open:
    /// this is a way out of them, not their end.
    pub fn exit_scopes(&mut self, depth: usize) -> Built {
        for scope in (depth..self.scopes.len()).rev() {
            for at in (0..self.scopes[scope].vars.len()).rev() {
                let var = self.scopes[scope].vars[at];
                self.b.storage_dead(var)?;
            }
        }
        Ok(())
    }

    /// A new variable of the innermost scope for the local that the
    /// checker knows.
    pub fn declare(&mut self, local: CheckedLocalId) -> Built<LocalId> {
        let declared = &self.body.locals[local.0 as usize];
        let ty = self.lower(declared.ty, declared.span);
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
            label.live = Some(scope.map_or(0, |scope| scope.vars.len()));
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
        if let (Some(live), Some(scope)) = (live, depth.checked_sub(1)) {
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
