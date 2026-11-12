//! Statements: blocks, branches, loops and what leaves them.

use gopp_sema::constant::Value;
use gopp_sema::types::Category;
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{
    Block, BranchKind, CaseClause, DeclId, DeclKind, ExprId, ExprKind, Ident, StmtId, StmtKind,
    UnaryOp,
};
use lasso::Spur;

use super::Shared;
use super::expr::PlaceMode;
use super::func::{Built, FuncCx, Target, deeper};
use crate::{BinOp, BlockId, CastKind, LocalId, Operand, Place, Rvalue, Type, TypeInterner};

/// What a range loop walks.
enum Walked {
    /// Nothing but the count: a number, or an array whose elements are
    /// not asked for.
    Count,
    /// The elements of the array in the place.
    Elements(Place),
    /// The code points of the string in the place.
    Text(Place),
}

impl FuncCx<'_, '_> {
    /// The statements of one list, whose labels are known before the
    /// first of them is lowered: a `goto` may come before its label.
    pub fn stmts(&mut self, list: &[StmtId]) -> Built {
        for &stmt in list {
            let mut labeled = stmt;
            while let StmtKind::Labeled { label, stmt } = &self.tree.stmts[labeled].kind {
                self.label(label.name);
                labeled = *stmt;
            }
        }
        for &stmt in list {
            self.stmt(stmt)?;
        }
        Ok(())
    }

    pub fn block(&mut self, block: &Block) -> Built {
        self.open_scope();
        self.stmts(&block.stmts)?;
        self.close_scope(block.span)
    }

    pub fn stmt(&mut self, id: StmtId) -> Built {
        deeper(|| self.stmt_here(id))
    }

    fn stmt_here(&mut self, id: StmtId) -> Built {
        self.memo.steps += 1;
        let node = &self.tree.stmts[id];
        self.at(node.span);
        let label = self.label.take();
        match &node.kind {
            StmtKind::Bad | StmtKind::Empty => {}
            StmtKind::Labeled { label, stmt } => {
                let block = self.label(label.name);
                self.b.goto(block)?;
                self.b.switch_to(block);
                self.label_met(label.name);
                self.label = Some(label.name);
                self.stmt(*stmt)?;
            }
            StmtKind::Block(block) => self.block(block)?,
            StmtKind::Expr(expr) => drop(self.rvalue(*expr)?),
            StmtKind::Decl(decls) => {
                for &decl in decls {
                    self.local_decl(decl)?;
                }
            }
            StmtKind::Assign { op, lhs, rhs } => self.assign(*op, lhs, rhs)?,
            StmtKind::IncDec { x, inc } => self.inc_dec(*x, *inc)?,
            StmtKind::Branch { kind, label } => self.branch(node.span, *kind, *label)?,
            StmtKind::Return(values) => self.ret(node.span, values)?,
            StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            } => self.if_stmt(node.span, *init, *cond, then, *otherwise)?,
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => self.for_stmt(label, node.span, *init, *cond, *post, body)?,
            StmtKind::Switch { init, tag, clauses } => {
                self.switch_stmt(label, node.span, *init, *tag, clauses)?;
            }
            StmtKind::Range {
                lhs,
                define,
                x,
                body,
            } => self.range_stmt(label, node.span, lhs, *define, *x, body)?,
            StmtKind::Go(_) => self.unsupported(node.span, "a go statement"),
            StmtKind::Defer(_) => self.unsupported(node.span, "a defer statement"),
            StmtKind::Send { .. } => self.unsupported(node.span, "a send"),
            StmtKind::Select { .. } => self.unsupported(node.span, "a select statement"),
        }
        Ok(())
    }

    /// A `var` statement; constants and types make no code.
    fn local_decl(&mut self, decl: DeclId) -> Built {
        if let DeclKind::Var { names, values, .. } = &self.tree.decl_nodes[decl].kind {
            self.define(names, values)?;
        }
        Ok(())
    }

    /// New variables with their first values, or with zeros.
    fn define(&mut self, names: &[Ident], values: &[ExprId]) -> Built {
        let local = |this: &Self, name: &Ident| this.body.defs.get(&name.span.lo).copied();
        if values.is_empty() {
            for name in names {
                if let Some(local) = local(self, name) {
                    let var = self.declare(local)?;
                    let zero = self.zero(self.b.place_ty(&Place::local(var)));
                    self.b.assign(Place::local(var), Rvalue::Use(zero))?;
                }
            }
            return Ok(());
        }
        let wanted: Vec<_> = names
            .iter()
            .map(|name| {
                let declared = local(self, name).map(|local| &self.body.locals[local.0 as usize]);
                declared.map(|declared| (declared.ty, declared.span))
            })
            .collect();
        let wanted: Vec<_> = wanted
            .into_iter()
            .map(|wanted| wanted.map(|(ty, span)| self.lower(ty, span)))
            .collect();
        let values = self.values(values, &wanted)?;
        for (name, value) in names.iter().zip(values) {
            if let Some(local) = local(self, name) {
                let var = self.declare(local)?;
                self.store(Place::local(var), value)?;
            }
        }
        Ok(())
    }

    fn if_stmt(
        &mut self,
        span: SourceSpan,
        init: Option<StmtId>,
        cond: ExprId,
        then: &Block,
        otherwise: Option<StmtId>,
    ) -> Built {
        self.open_scope();
        if let Some(init) = init {
            self.stmt(init)?;
        }
        let (yes, done) = (self.b.new_block(), self.b.new_block());
        let no = match otherwise {
            Some(_) => self.b.new_block(),
            None => done,
        };
        self.at(self.tree.exprs[cond].span);
        self.cond(cond, yes, no)?;
        self.b.switch_to(yes);
        self.block(then)?;
        self.b.goto(done)?;
        if let Some(otherwise) = otherwise {
            self.b.switch_to(no);
            self.stmt(otherwise)?;
            self.b.goto(done)?;
        }
        self.b.switch_to(done);
        self.close_scope(span)
    }

    fn for_stmt(
        &mut self,
        label: Option<Spur>,
        span: SourceSpan,
        init: Option<StmtId>,
        cond: Option<ExprId>,
        post: Option<StmtId>,
        body: &Block,
    ) -> Built {
        self.open_scope();
        if let Some(init) = init {
            self.stmt(init)?;
        }
        let [head, inside, after, exit] = [(); 4].map(|()| self.b.new_block());
        self.b.goto(head)?;
        self.b.switch_to(head);
        match cond {
            Some(cond) => {
                self.at(self.tree.exprs[cond].span);
                self.cond(cond, inside, exit)?;
            }
            None => self.b.goto(inside)?,
        }
        self.b.switch_to(inside);
        self.targets.push(Target {
            label,
            breaks: exit,
            continues: Some(after),
            depth: self.depth(),
            next: None,
        });
        self.block(body)?;
        self.targets.pop();
        self.b.goto(after)?;
        self.b.switch_to(after);
        if let Some(post) = post {
            self.stmt(post)?;
        }
        self.b.goto(head)?;
        self.b.switch_to(exit);
        self.close_scope(span)
    }

    /// `for key, value := range x` over a number, an array or a string.
    /// What is walked is worked out once, before the first pass; each
    /// pass has its own variables.
    fn range_stmt(
        &mut self,
        label: Option<Spur>,
        span: SourceSpan,
        lhs: &[ExprId],
        define: bool,
        x: ExprId,
        body: &Block,
    ) -> Built {
        let inner = |this: &Self, left: ExprId| Shared::unparen(this.tree, left);
        let alias = |&left: &ExprId| {
            let kind = &self.tree.exprs[inner(self, left)].kind;
            let address = UnaryOp::Addr;
            matches!(kind, ExprKind::Unary { op, .. } if *op == address)
        };
        if lhs.iter().any(alias) {
            self.unsupported(span, "a range variable that is the element itself");
            return Ok(());
        }
        let (key, value) = (lhs.first().copied(), lhs.get(1).copied());
        let named = value.is_some_and(|value| {
            let kind = &self.tree.exprs[inner(self, value)].kind;
            !matches!(kind, ExprKind::Name(name) if *name == self.shared.blank)
        });
        self.open_scope();
        self.at(self.tree.exprs[x].span);
        let word = super::Memo::word(self.shared, self.b.types(), false);
        let slot = |local: LocalId| Operand::Copy(Place::local(local));
        let walked = self.ty_of(x);
        // What is walked, the number of passes or what tells it, and
        // the type that the loop counts in.
        let behind = self.pointee(walked);
        // A constant without a type is a count; anything else without a
        // type of GIR has a type that is not supported, which was said.
        let constant = self.body.types.get(&x);
        let constant = constant.is_some_and(|typed| typed.value != Value::Unknown);
        let (source, limit, counts) = match self.kind(behind) {
            Type::Unit if !constant => {
                self.unsupported(span, "a range over a value of this type");
                return self.close_scope(span);
            }
            Type::Int { .. } | Type::Unit if behind == walked => {
                let counts = match walked == TypeInterner::UNIT {
                    true => super::Memo::word(self.shared, self.b.types(), true),
                    false => walked,
                };
                let limit = self.operand_as(x, counts)?;
                let limit = self.b.assign_temp(Rvalue::Use(limit))?;
                (Walked::Count, slot(limit), counts)
            }
            // The length of an array is known: without a variable for
            // the elements the array is not worked out at all.
            Type::Array(_, len) => {
                let limit = self.b.const_int(word, len)?;
                let source = match named {
                    true => Walked::Elements(self.ranged_array(x, span)?),
                    false => Walked::Count,
                };
                (source, limit, word)
            }
            Type::String => {
                let whole = self.operand(x)?;
                let whole = Place::local(self.b.assign_temp(Rvalue::Use(whole))?);
                let len = self.b.assign_temp(Rvalue::Len(whole.clone()))?;
                (Walked::Text(whole), slot(len), word)
            }
            // The elements of the slice as it is when the loop starts:
            // the body may write them, and may not grow the slice.
            Type::Slice(of) => {
                if named && !self.flat(of) {
                    self.unsupported(span, "a copy of elements that hold a slice");
                    return self.close_scope(span);
                }
                let mut mode = PlaceMode::default();
                let place = self.place(x, &mut mode)?;
                let place = self.through(place, span, &mut mode)?;
                let whole = self.b.assign_temp(Rvalue::Use(Operand::Copy(place)))?;
                let whole = Place::local(whole);
                let len = self.b.assign_temp(Rvalue::Len(whole.clone()))?;
                (Walked::Elements(whole), slot(len), word)
            }
            _ => {
                self.unsupported(span, "a range over a map, a channel or a function");
                return self.close_scope(span);
            }
        };
        let counter = self.b.assign_temp(Rvalue::Use(self.zero(counts)))?;
        let [head, inside, after, exit] = [(); 4].map(|()| self.b.new_block());
        self.b.goto(head)?;
        self.b.switch_to(head);
        let more = Rvalue::Binary(BinOp::Lt, slot(counter), limit);
        let more = self.b.assign_temp(more)?;
        self.b.switch(slot(more), vec![(0, exit)], inside)?;
        self.b.switch_to(inside);
        let target = Target {
            label,
            breaks: exit,
            continues: Some(after),
            depth: self.depth(),
            next: None,
        };
        self.open_scope();
        // What this pass walks, and how far the next one starts.
        let mut step = self.b.const_int(counts, 1)?;
        let element = match &source {
            Walked::Count => None,
            Walked::Elements(array) => Some(Operand::Copy(self.b.index(array, counter)?)),
            Walked::Text(text) => {
                let string = self.b.place_ty(text);
                let args = vec![Operand::Copy(text.clone()), slot(counter)];
                let packed = self.runtime("_gopp_string_decode", &[string, word], word, args)?;
                let rune = self.b.types().int(32, true);
                let point = Rvalue::Cast(CastKind::IntToInt, packed.clone(), rune);
                let point = self.b.assign_temp(point)?;
                let half = self.b.const_int(word, 32)?;
                let width = self
                    .b
                    .assign_temp(Rvalue::Binary(BinOp::Shr, packed, half))?;
                step = slot(width);
                Some(slot(point))
            }
        };
        if let Some(key) = key {
            self.set_range_var(key, define, slot(counter))?;
        }
        if let (Some(value), Some(element)) = (value, element) {
            self.set_range_var(value, define, element)?;
        }
        self.targets.push(target);
        self.block(body)?;
        self.targets.pop();
        self.close_scope(span)?;
        self.b.goto(after)?;
        self.b.switch_to(after);
        let next = Rvalue::Binary(BinOp::Add, slot(counter), step);
        self.b.assign(Place::local(counter), next)?;
        self.b.goto(head)?;
        self.b.switch_to(exit);
        self.close_scope(span)
    }

    /// The array whose elements a range loop reads. An array whose
    /// elements are copied by their bytes is copied first, as in Go: the
    /// loop does not see what its body writes. Any other, and one behind
    /// a pointer, is walked where it is (RNG-6).
    fn ranged_array(&mut self, x: ExprId, span: SourceSpan) -> Built<Place> {
        let mut mode = PlaceMode::default();
        let place = self.place(x, &mut mode)?;
        let pointer = matches!(self.kind(self.b.place_ty(&place)), Type::Ptr(_));
        let checked = self.body.types.get(&x).map(|typed| typed.ty);
        let trivial = checked.is_some_and(|ty| self.shared.types.category(ty) == Category::Trivial);
        match (pointer, trivial) {
            (true, _) => self.deref(place, span, &mut mode),
            (false, true) => {
                let copy = self.b.assign_temp(Rvalue::Use(Operand::Copy(place)))?;
                Ok(Place::local(copy))
            }
            (false, false) => Ok(place),
        }
    }

    fn switch_stmt(
        &mut self,
        label: Option<Spur>,
        span: SourceSpan,
        init: Option<StmtId>,
        tag: Option<ExprId>,
        clauses: &[CaseClause],
    ) -> Built {
        let guard =
            |tag: &ExprId| matches!(self.tree.exprs[*tag].kind, ExprKind::TypeSwitchGuard { .. });
        if tag.as_ref().is_some_and(guard) {
            self.unsupported(span, "a type switch");
            return Ok(());
        }
        self.open_scope();
        if let Some(init) = init {
            self.stmt(init)?;
        }
        let exit = self.b.new_block();
        let bodies: Vec<BlockId> = clauses.iter().map(|_| self.b.new_block()).collect();
        let default = clauses.iter().position(|clause| clause.cases.is_none());
        let otherwise = default.map_or(exit, |at| bodies[at]);
        let cases = clauses.iter().zip(&bodies).flat_map(|(clause, &body)| {
            let cases = clause.cases.iter().flatten();
            cases.map(move |&case| (case, body))
        });
        let cases: Vec<(ExprId, BlockId)> = cases.collect();
        match tag {
            Some(tag) => {
                self.at(self.tree.exprs[tag].span);
                self.switch_on(tag, &cases, otherwise)?;
            }
            None => {
                for &(case, body) in &cases {
                    let next = self.b.new_block();
                    self.cond(case, body, next)?;
                    self.b.switch_to(next);
                }
                self.b.goto(otherwise)?;
            }
        }
        for (at, clause) in clauses.iter().enumerate() {
            self.b.switch_to(bodies[at]);
            self.open_scope();
            self.targets.push(Target {
                label,
                breaks: exit,
                continues: None,
                depth: self.depth() - 1,
                next: bodies.get(at + 1).copied(),
            });
            self.stmts(&clause.body)?;
            self.targets.pop();
            self.close_scope(clause.span)?;
            self.b.goto(exit)?;
        }
        self.b.switch_to(exit);
        self.close_scope(span)
    }

    /// Goes to the body of the case that the tag is equal to: by one
    /// table if the cases are integers that are known, by one
    /// comparison after another if not.
    fn switch_on(&mut self, tag: ExprId, cases: &[(ExprId, BlockId)], otherwise: BlockId) -> Built {
        let ty = self.ty_of(tag);
        let on = self.operand_as(tag, ty)?;
        let on = self.stable(on)?;
        let known = |this: &Self, case: ExprId| match this.body.types.get(&case) {
            Some(typed) if !this.body.case_operators.contains_key(&case) => {
                typed.value.to_int().low_bits()
            }
            _ => None,
        };
        let values: Option<Vec<u64>> = cases.iter().map(|&(case, _)| known(self, case)).collect();
        if let (Type::Int { bits, .. }, Some(values)) = (self.kind(ty), values) {
            let mask = match bits {
                64.. => u64::MAX,
                bits => (1 << bits) - 1,
            };
            let mut table: Vec<(u64, BlockId)> = Vec::with_capacity(cases.len());
            let mut seen = rustc_hash::FxHashSet::default();
            for (value, &(_, body)) in values.into_iter().zip(cases) {
                if seen.insert(value & mask) {
                    table.push((value & mask, body));
                }
            }
            return self.b.switch(on, table, otherwise);
        }
        let compared = matches!(
            self.kind(ty),
            Type::Int { .. } | Type::Float { .. } | Type::Bool | Type::Ptr(_) | Type::String
        );
        for &(case, body) in cases {
            let span = self.tree.exprs[case].span;
            if !compared || self.body.case_operators.contains_key(&case) {
                self.unsupported(span, "a case that is compared by a function");
                continue;
            }
            let value = self.operand_as(case, ty)?;
            let equal = self.arith(
                span,
                gopp_syntax::ast::BinaryOp::Eq,
                on.clone(),
                value,
                false,
            )?;
            let equal = self.b.assign_temp(equal)?;
            let next = self.b.new_block();
            let equal = Operand::Copy(Place::local(equal));
            self.b.switch(equal, vec![(0, next)], body)?;
            self.b.switch_to(next);
        }
        self.b.goto(otherwise)
    }

    fn branch(&mut self, span: SourceSpan, kind: BranchKind, label: Option<Ident>) -> Built {
        let label = label.map(|label| label.name);
        let named = |target: &&Target| label.is_none() || target.label == label;
        let (to, depth) = match kind {
            BranchKind::Break => {
                let found = self.targets.iter().rev().find(named);
                (
                    found.map(|target| target.breaks),
                    found.map(|target| target.depth),
                )
            }
            BranchKind::Continue => {
                let loops = self.targets.iter().rev();
                let mut loops = loops.filter(|target| target.continues.is_some());
                let found = loops.find(named);
                (
                    found.and_then(|target| target.continues),
                    found.map(|target| target.depth),
                )
            }
            BranchKind::Fallthrough => {
                let clause = self.targets.last();
                (
                    clause.and_then(|target| target.next),
                    clause.map(|target| target.depth),
                )
            }
            BranchKind::Goto => {
                let to = match label {
                    Some(label) => self.exit_to_label(label)?,
                    None => None,
                };
                (to, None)
            }
        };
        let Some(to) = to else {
            self.unsupported(span, "a jump to nowhere");
            return Ok(());
        };
        if let Some(depth) = depth {
            self.exit_scopes(depth)?;
        }
        self.b.goto(to)?;
        self.dead();
        Ok(())
    }

    fn ret(&mut self, span: SourceSpan, values: &[ExprId]) -> Built {
        let result = Place::local(self.b.result());
        let ty = self.b.place_ty(&result);
        let parts: Vec<_> = match self.kind(ty) {
            Type::Tuple(parts) if self.results.len() > 1 => parts.to_vec(),
            _ if self.results.is_empty() => Vec::new(),
            _ => vec![ty],
        };
        let named: Vec<_> = self.results.clone();
        let mut given: Vec<Rvalue> = Vec::with_capacity(parts.len());
        if values.is_empty() {
            for (var, &part) in named.iter().zip(&parts) {
                given.push(Rvalue::Use(match var {
                    Some(var) => Operand::Copy(Place::local(*var)),
                    None => self.zero(part),
                }));
            }
        } else {
            let wanted: Vec<_> = parts.iter().map(|&part| Some(part)).collect();
            given = self.values(values, &wanted)?;
            // The variables of the results have the values too: what
            // runs after the `return` reads them.
            if named.iter().any(Option::is_some) {
                // A result that is returned as it is stays where it is.
                let itself = |var: &Option<crate::LocalId>, value: &Rvalue| {
                    let own = var.map(|var| Rvalue::Use(Operand::Copy(Place::local(var))));
                    own.as_ref() == Some(value)
                };
                let mut held = Vec::with_capacity(given.len());
                for (var, value) in named.iter().zip(given) {
                    let own = itself(var, &value);
                    let mut value = self.operand_of(value)?;
                    if !own {
                        value = self.stable(value)?;
                    }
                    held.push((own, value));
                }
                for (var, (own, value)) in named.iter().zip(&held) {
                    if let (Some(var), false) = (var, own) {
                        self.b
                            .assign(Place::local(*var), Rvalue::Use(value.clone()))?;
                    }
                }
                given = held
                    .into_iter()
                    .map(|(_, value)| Rvalue::Use(value))
                    .collect();
            }
        }
        self.at(span);
        match given.len() {
            0 => {}
            1 => {
                let value = given.swap_remove(0);
                self.b.assign(result, value)?;
            }
            _ => {
                let mut parts = Vec::with_capacity(given.len());
                for value in given {
                    parts.push(self.operand_of(value)?);
                }
                let whole = Rvalue::Aggregate(crate::AggKind::Tuple, parts);
                self.b.assign(result, whole)?;
            }
        }
        self.exit_scopes(0)?;
        self.b.ret()?;
        self.dead();
        Ok(())
    }

    /// Whether the expression is known to be true or false.
    pub fn known_bool(&self, expr: ExprId) -> Option<bool> {
        match self.body.types.get(&expr)?.value {
            Value::Bool(known) => Some(known),
            _ => None,
        }
    }

    /// Goes to `yes` if the condition holds and to `no` if not. Of
    /// `a && b` and `a || b` the right side is evaluated only when it
    /// decides.
    pub fn cond(&mut self, expr: ExprId, yes: BlockId, no: BlockId) -> Built {
        deeper(|| self.cond_here(expr, yes, no))
    }

    fn cond_here(&mut self, expr: ExprId, yes: BlockId, no: BlockId) -> Built {
        self.memo.steps += 1;
        if let Some(known) = self.known_bool(expr) {
            return self.b.goto(if known { yes } else { no });
        }
        let node = &self.tree.exprs[expr];
        let plain = !self.body.operators.contains_key(&expr);
        match node.kind {
            ExprKind::Paren(inner) => self.cond(inner, yes, no),
            ExprKind::Binary {
                op: gopp_syntax::ast::BinaryOp::AndAnd,
                x,
                y,
            } if plain => {
                let both = self.b.new_block();
                self.cond(x, both, no)?;
                self.b.switch_to(both);
                self.cond(y, yes, no)
            }
            ExprKind::Binary {
                op: gopp_syntax::ast::BinaryOp::OrOr,
                x,
                y,
            } if plain => {
                let either = self.b.new_block();
                self.cond(x, yes, either)?;
                self.b.switch_to(either);
                self.cond(y, yes, no)
            }
            ExprKind::Unary {
                op: gopp_syntax::ast::UnaryOp::Not,
                x,
            } if plain => self.cond(x, no, yes),
            _ => {
                let holds = self.operand_as(expr, TypeInterner::BOOL)?;
                self.b.switch(holds, vec![(0, no)], yes)
            }
        }
    }
}
