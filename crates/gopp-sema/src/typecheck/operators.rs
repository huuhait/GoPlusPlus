//! Operator methods (ADR-0024): the `@Operator` methods of a type stand
//! for its operators, those of one name told apart by the types of their
//! parameters; `@Convert` methods stand for conversions (EXP-16 to
//! EXP-30, TYP-27, GEN-13).

use super::*;
use crate::exprstring::binary_text;
use crate::object::{OperatorUse, SelectionKind};
use crate::types::{Marks, NamedId, Operator};

/// What a method of one operator name looks like.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Shape {
    /// `(b U) R`, value receiver.
    Binary,
    /// `() R`, value receiver.
    Unary,
    /// `(b U)`, pointer receiver.
    Assign,
    /// `(b U) bool`, value receiver.
    Eq,
    /// `(b U) int`, value receiver.
    Cmp,
    /// `(i I, …) R`.
    Index,
    /// `(i I, …, v V)`, pointer receiver.
    SetIndex,
    /// `() F`, where `range` accepts an `F`.
    Iter,
}

/// The names of the methods that `@Operator` marks, each with the
/// operator as it is written and its shape.
pub(super) const OPERATORS: [(&str, &str, Shape); 30] = [
    ("Add", "+", Shape::Binary),
    ("Sub", "-", Shape::Binary),
    ("Mul", "*", Shape::Binary),
    ("Div", "/", Shape::Binary),
    ("Rem", "%", Shape::Binary),
    ("BitAnd", "&", Shape::Binary),
    ("BitOr", "|", Shape::Binary),
    ("BitXor", "^", Shape::Binary),
    ("BitClear", "&^", Shape::Binary),
    ("Shl", "<<", Shape::Binary),
    ("Shr", ">>", Shape::Binary),
    ("Eq", "==", Shape::Eq),
    ("Cmp", "<", Shape::Cmp),
    ("Neg", "-", Shape::Unary),
    ("Not", "!", Shape::Unary),
    ("BitNot", "^", Shape::Unary),
    ("AddAssign", "+=", Shape::Assign),
    ("SubAssign", "-=", Shape::Assign),
    ("MulAssign", "*=", Shape::Assign),
    ("DivAssign", "/=", Shape::Assign),
    ("RemAssign", "%=", Shape::Assign),
    ("BitAndAssign", "&=", Shape::Assign),
    ("BitOrAssign", "|=", Shape::Assign),
    ("BitXorAssign", "^=", Shape::Assign),
    ("BitClearAssign", "&^=", Shape::Assign),
    ("ShlAssign", "<<=", Shape::Assign),
    ("ShrAssign", ">>=", Shape::Assign),
    ("Index", "[]", Shape::Index),
    ("SetIndex", "[]=", Shape::SetIndex),
    ("Iter", "range", Shape::Iter),
];

/// The operators whose operands may change places: one `@Right` method
/// serves both orders.
const COMMUTATIVE: [&str; 6] = ["Add", "Mul", "BitAnd", "BitOr", "BitXor", "Eq"];

pub(super) fn shape_of(name: &str) -> Option<Shape> {
    let row = OPERATORS.iter().find(|row| row.0 == name);
    row.map(|row| row.2)
}

/// Whether a method of the name has an operand on each side.
pub(super) fn two_operands(name: &str) -> bool {
    matches!(shape_of(name), Some(Shape::Binary | Shape::Eq | Shape::Cmp))
}

/// The name of the method that stands for the binary operator.
fn binary_method(op: BinaryOp) -> Option<&'static str> {
    Some(match op {
        BinaryOp::Add => "Add",
        BinaryOp::Sub => "Sub",
        BinaryOp::Mul => "Mul",
        BinaryOp::Div => "Div",
        BinaryOp::Rem => "Rem",
        BinaryOp::And => "BitAnd",
        BinaryOp::Or => "BitOr",
        BinaryOp::Xor => "BitXor",
        BinaryOp::AndNot => "BitClear",
        BinaryOp::Shl => "Shl",
        BinaryOp::Shr => "Shr",
        BinaryOp::Eq | BinaryOp::Ne => "Eq",
        BinaryOp::Lt | BinaryOp::Le | BinaryOp::Gt | BinaryOp::Ge => "Cmp",
        BinaryOp::AndAnd | BinaryOp::OrOr => return None,
    })
}

/// What stands for a binary operator.
enum Choice {
    Builtin,
    Refused,
    /// The method, and whether its receiver is the right operand.
    Method(Operator, bool),
}

/// How an operator expression is decided.
pub(super) enum Overload {
    /// No method stands for it: it has the meaning that Go gives it.
    Builtin,
    /// It is in error, which was reported.
    Refused,
    /// A method stands for it, and this is what it gives.
    Chosen(Operand),
}

/// A method declaration that `@Operator` or `@Convert` marks.
pub(super) struct Marked<'m> {
    pub named: NamedId,
    /// The receiver type, written with the type parameters that the
    /// receiver names.
    pub receiver: TypeId,
    pub name: Ident,
    pub method: &'m DeclaredMethod,
    /// It has type parameters of its own.
    pub generic: bool,
    /// It is declared with `!`.
    pub fails: bool,
}

impl<'a> Checker<'a> {
    pub(super) fn func_parts(&self, sig: TypeId) -> (Vec<TypeId>, Vec<TypeId>, bool) {
        match &*self.types.get(sig) {
            Type::Func {
                params,
                results,
                variadic,
                ..
            } => (params.to_vec(), results.to_vec(), *variadic),
            _ => (Vec::new(), Vec::new(), false),
        }
    }

    /// Checks the declaration of an `@Operator` method and adds it to
    /// the methods of its name (EXP-16, EXP-17).
    pub(super) fn operator_declared(&mut self, marked: &Marked<'_>) {
        let method = marked.method;
        let name = self.names.resolve(&marked.name.name);
        let Some(&(_, symbol, shape)) = OPERATORS.iter().find(|row| row.0 == name) else {
            return;
        };
        let at = marked.name.span;
        let sig = method.method.sig;
        if sig == Types::INVALID {
            return;
        }
        let invalid = |what: &str| format!("invalid @Operator method {name}: {what}");
        if marked.generic {
            let message = invalid("an operator method has no type parameters of its own");
            return self.error(Code::OpTypeParams, at, message);
        }
        if marked.fails {
            let message = invalid("an operator method cannot fail");
            return self.error(Code::OpFails, at, message);
        }
        let (params, results, variadic) = self.func_parts(sig);
        let (bool, int) = (self.types.basic(Basic::Bool), self.types.basic(Basic::Int));
        let wrong = match shape {
            _ if variadic => true,
            Shape::Binary => params.len() != 1 || results.len() != 1,
            Shape::Unary => !params.is_empty() || results.len() != 1,
            Shape::Assign => params.len() != 1 || !results.is_empty(),
            Shape::Eq => params.len() != 1 || results[..] != [bool],
            Shape::Cmp => params.len() != 1 || results[..] != [int],
            Shape::Index => params.is_empty() || results.len() != 1,
            Shape::SetIndex => params.len() < 2 || !results.is_empty(),
            Shape::Iter => {
                let ranged = match &results[..] {
                    [result] => {
                        let under = self.types.underlying(*result);
                        matches!(&*self.types.get(under), Type::Func { .. })
                            && matches!(self.range_key_value(*result), Ok(Some(_)))
                    }
                    _ => false,
                };
                !params.is_empty() || !ranged
            }
        };
        if wrong {
            let want = match shape {
                Shape::Binary => "its form is (b U) R",
                Shape::Unary => "its form is () R",
                Shape::Assign => "its form is (b U), without a result",
                Shape::Eq => "its form is (b U) bool",
                Shape::Cmp => "its form is (b U) int",
                Shape::Index => "its form is (i I, …) R",
                Shape::SetIndex => "its form is (i I, …, v V), without a result",
                Shape::Iter => "its form is () F, for a function type F that range accepts",
            };
            return self.error(Code::OpShape, at, invalid(want));
        }
        let wrong = match shape {
            Shape::Binary | Shape::Unary | Shape::Eq | Shape::Cmp if method.pointer_receiver => {
                Some("it needs a value receiver: its operand may be a temporary")
            }
            Shape::Assign | Shape::SetIndex if !method.pointer_receiver => {
                Some("it needs a pointer receiver: it changes its operand")
            }
            _ => None,
        };
        if let Some(wrong) = wrong {
            return self.error(Code::OpReceiver, at, invalid(wrong));
        }
        let right = method.marks.has(Marks::RIGHT);
        if right && params.len() == 1 && self.types.identical(params[0], marked.receiver) {
            let message = "invalid @Right: the parameter has the type of the receiver".to_owned();
            return self.error(Code::RightMisuse, at, message);
        }
        if let Some(under) = self.second_meaning(marked.receiver, name, shape, &params) {
            let message = format!(
                "{} already has the operator {symbol}: its underlying type {} gives it",
                self.type_string(marked.receiver),
                self.type_string(under)
            );
            return self.error(Code::OpSecondMeaning, at, invalid(&message));
        }
        // Two methods of one name take other parameters, or stand for
        // the two orders of an operator whose operands do not change
        // places.
        let both_orders = COMMUTATIVE.contains(&name);
        let declared = self.operator_decls.entry(marked.named).or_default();
        let again = declared.iter().find(|known| {
            known.name == marked.name.name
                && (both_orders || known.right == right)
                && self.types.same_params(known.sig, sig)
        });
        if let Some(first) = again.map(|known| known.span) {
            let message = format!(
                "method {}.{name} already declared for these parameter types",
                self.names.resolve(&self.types.name_of(marked.named))
            );
            let note = format!("other declaration of method {name}");
            let diagnostic = Diagnostic::new(Code::OpDuplicate, at, message);
            self.emit(diagnostic.with_note(Code::NoteOtherDeclaration, Some(first), note));
            return;
        }
        let declared = self.operator_decls.entry(marked.named).or_default();
        self.operator_spans
            .insert((marked.named, declared.len() as u32), at);
        declared.push(OperatorDecl {
            name: marked.name.name,
            sig,
            right,
            span: at,
        });
        self.types.add_operator(marked.named, method.clone());
    }

    /// The underlying type that gives a type the operator already, if a
    /// method of the name with the parameters would be a second meaning.
    fn second_meaning(
        &self,
        receiver: TypeId,
        name: &str,
        shape: Shape,
        params: &[TypeId],
    ) -> Option<TypeId> {
        let under = self.types.underlying(receiver);
        let own = |param: &TypeId| self.types.identical(*param, receiver);
        let arithmetic = |name: &str| match name {
            "Add" => self.is_numeric(under) || self.is_string(under),
            "Sub" | "Mul" | "Div" => self.is_numeric(under),
            "Shl" | "Shr" => false,
            _ => self.is_integer(under),
        };
        let ordered = self.is_string(under) || (self.is_numeric(under) && !self.is_complex(under));
        let is_basic = matches!(&*self.types.get(under), Type::Basic(_));
        let has = match shape {
            Shape::Binary | Shape::Assign => {
                let binary = name.strip_suffix("Assign").unwrap_or(name);
                match binary {
                    "Shl" | "Shr" => self.is_integer(under) && self.is_integer(params[0]),
                    _ => own(&params[0]) && arithmetic(binary),
                }
            }
            Shape::Eq => own(&params[0]) && is_basic,
            Shape::Cmp => own(&params[0]) && ordered,
            Shape::Unary => match name {
                "Neg" => self.is_numeric(under),
                "Not" => self.is_boolean(under),
                _ => self.is_integer(under),
            },
            Shape::Index | Shape::SetIndex | Shape::Iter => {
                let pointee = match &*self.types.get(under) {
                    Type::Pointer(elem) => self.types.underlying(*elem),
                    _ => under,
                };
                let ranged = shape == Shape::Iter;
                match &*self.types.get(pointee) {
                    Type::Array { .. } => true,
                    _ if pointee != under => false,
                    Type::Slice(_) | Type::Map { .. } => true,
                    Type::Basic(_) => self.is_string(under) || (ranged && self.is_integer(under)),
                    Type::Chan { .. } | Type::Func { .. } => ranged,
                    _ => false,
                }
            }
        };
        has.then_some(under)
    }

    /// Checks the declaration of a `@Convert` method: one for each type
    /// that it converts to, and none where Go converts already (EXP-25).
    pub(super) fn convert_declared(&mut self, marked: &Marked<'_>) {
        let (_, results, _) = self.func_parts(marked.method.method.sig);
        let [target] = results[..] else {
            return;
        };
        let name = self.names.resolve(&marked.name.name);
        let at = marked.name.span;
        let from = Operand::value(marked.receiver);
        if self.convertible(&from, target, &mut None) {
            let message = format!(
                "invalid @Convert method {name}: a {} converts to {} already",
                self.type_string(marked.receiver),
                self.type_string(target)
            );
            return self.error(Code::OpSecondMeaning, at, message);
        }
        let declared = self.convert_decls.entry(marked.named).or_default();
        let again = declared
            .iter()
            .find(|known| self.types.identical(known.0, target));
        if let Some(&(_, first)) = again {
            let message = format!(
                "invalid @Convert method {name}: {} has a @Convert method to {} already",
                self.type_string(marked.receiver),
                self.type_string(target)
            );
            let diagnostic = Diagnostic::new(Code::ConvertMisuse, at, message);
            let note = "the other @Convert method".to_owned();
            self.emit(diagnostic.with_note(Code::NoteOtherDeclaration, Some(first), note));
            return;
        }
        self.convert_decls
            .entry(marked.named)
            .or_default()
            .push((target, at));
    }

    /// A method that is called by its name is the one of the `@Operator`
    /// methods of the name that takes the arguments (EXP-30). `callee`
    /// is what the selector `called` gave.
    pub(super) fn operator_by_name(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        called: ExprId,
        callee: Operand,
        args: &[ExprId],
    ) -> Operand {
        let ExprKind::Selector { sel, .. } = cx.tree.exprs[called].kind else {
            return callee;
        };
        let holder = match self.selections.get(&called) {
            Some(selection) if selection.kind == SelectionKind::Method => selection.holder,
            _ => return callee,
        };
        let all = self.types.operators(holder, sel.name);
        if all.len() < 2 {
            return callee;
        }
        // A method for the other order of operands that do not change
        // places is called by the operator alone.
        let name = self.names.resolve(&sel.name);
        let both_orders = COMMUTATIVE.contains(&name);
        let methods: Vec<Operator> = all
            .into_iter()
            .filter(|method| !method.marks.has(Marks::RIGHT) || both_orders)
            .collect();
        let operands: Vec<(ExprId, Operand)> =
            args.iter().map(|&arg| (arg, self.value(cx, arg))).collect();
        if operands.iter().any(|(_, arg)| arg.mode == Mode::Unknown) {
            return Operand::UNKNOWN;
        }
        let given: Vec<Operand> = operands.iter().map(|(_, arg)| arg.clone()).collect();
        let at = self.span(cx, called);
        let method = match self.pick(&methods, &given, true) {
            Ok(Some(at)) => methods[at].clone(),
            Ok(None) => {
                let types: Vec<String> = given
                    .iter()
                    .map(|arg| self.type_string(self.types.default_type(arg.ty)))
                    .collect();
                let message = format!(
                    "no @Operator {name} method of {} takes ({})",
                    self.type_string(holder),
                    types.join(", ")
                );
                self.error(Code::OpSelect, at, message);
                return Operand::UNKNOWN;
            }
            Err(()) => {
                let message = format!(
                    "more than one @Operator {name} method of {} takes these arguments",
                    self.type_string(holder)
                );
                self.error(Code::OpAmbiguous, at, message);
                return Operand::UNKNOWN;
            }
        };
        let used = self.operator_used(cx, at, &method, &operands, false);
        self.operators.insert(call, used);
        Operand::value(method.sig)
    }

    /// A method value names one method: not one of several `@Operator`
    /// methods of a name.
    pub(super) fn operator_value(&mut self, cx: &Cx<'a>, expr: ExprId, name: Spur) {
        let Some(holder) = self.selections.get(&expr).map(|selection| selection.holder) else {
            return;
        };
        if self.types.operators(holder, name).len() < 2 {
            return;
        }
        let message = format!(
            "{} names more than one @Operator method: only a call says which",
            self.expr_string(cx, expr)
        );
        self.error(Code::OpSelect, self.span(cx, expr), message);
    }

    /// Whether the type argument `v` meets the method `method` of a
    /// constraint as an operator element: with an `@Operator` method of
    /// that type, or with the operator that Go gives it. The `@Operator`
    /// methods of the name decide for a type that has some; `None` where
    /// the plain methods of the type decide (GEN-13). `same` says whether
    /// two method types are the same.
    pub(super) fn element_met(
        &self,
        v: TypeId,
        method: &Method,
        same: &mut dyn FnMut(TypeId, TypeId) -> bool,
    ) -> Option<bool> {
        let (sigs, decided) = self.element_sigs(v, method);
        let met = sigs.into_iter().any(|sig| same(sig, method.sig));
        (met || decided).then_some(met)
    }

    /// The types of the methods that stand for the method `method` of a
    /// constraint where the type argument is `v`: those of the
    /// `@Operator` methods of that name, and that of the operator that Go
    /// gives the type; and whether the type has `@Operator` methods of
    /// the name at all.
    fn element_sigs(&self, v: TypeId, method: &Method) -> (Vec<TypeId>, bool) {
        let name = self.names.resolve(&method.name);
        let Some(shape) = shape_of(name) else {
            return (Vec::new(), false);
        };
        let (params, ..) = self.func_parts(method.sig);
        let under = self.types.underlying(v);
        // A pointer has the methods of what it points to for indexing
        // and ranging only: the other elements name the type itself.
        let pointee = match &*self.types.get(under) {
            Type::Pointer(elem) => Some(*elem),
            _ => None,
        };
        let reaches = matches!(shape, Shape::Index | Shape::SetIndex | Shape::Iter);
        let holder = pointee.filter(|_| reaches).unwrap_or(v);
        let own = self.types.operators(holder, method.name);
        let both_orders = COMMUTATIVE.contains(&name);
        // A value has the methods with a value receiver; an in-place
        // method changes a place.
        let fits = |operator: &&Operator| {
            let receiver =
                !operator.pointer_receiver || pointee.is_some() || shape == Shape::Assign;
            receiver && (!operator.marks.has(Marks::RIGHT) || both_orders)
        };
        let mut sigs: Vec<TypeId> = own
            .iter()
            .filter(fits)
            .map(|operator| operator.sig)
            .collect();
        let (bool, int) = (self.types.basic(Basic::Bool), self.types.basic(Basic::Int));
        let arithmetic = |name: &str| match name {
            "Add" => self.is_numeric(under) || self.is_string(under),
            "Sub" | "Mul" | "Div" => self.is_numeric(under),
            _ => self.is_integer(under),
        };
        // The operand on the right of Go's operator: a value of the type
        // itself, or the count of a shift, of any integer type.
        let operand = |name: &str| match (name, &params[..]) {
            ("Shl" | "Shr", [count]) if self.is_integer(*count) => Some(*count),
            ("Shl" | "Shr", _) => None,
            _ => Some(v),
        };
        let ordered = self.is_string(under) || (self.is_numeric(under) && !self.is_complex(under));
        // The type of the elements of what is indexed with an `int`.
        let indexed = |by_value: bool| {
            let held = pointee.map_or(under, |elem| self.types.underlying(elem));
            match &*self.types.get(held) {
                Type::Slice(elem) | Type::Array { elem, .. } if by_value || pointee.is_some() => {
                    Some(*elem)
                }
                Type::Basic(_) if by_value && pointee.is_none() && self.is_string(under) => {
                    Some(self.types.byte())
                }
                _ => None,
            }
        };
        let given = match shape {
            Shape::Binary => operand(name)
                .filter(|_| arithmetic(name))
                .map(|operand| self.types.func(&[operand], &[v], false)),
            Shape::Assign => {
                let binary = name.strip_suffix("Assign").unwrap_or(name);
                operand(binary)
                    .filter(|_| arithmetic(binary))
                    .map(|operand| self.types.func(&[operand], &[], false))
            }
            Shape::Unary => {
                let has = match name {
                    "Neg" => self.is_numeric(under),
                    "Not" => self.is_boolean(under),
                    _ => self.is_integer(under),
                };
                has.then(|| self.types.func(&[], &[v], false))
            }
            // A type with an `Eq` method of its own is compared with it.
            Shape::Eq => (own.is_empty() && self.types.comparable(v))
                .then(|| self.types.func(&[v], &[bool], false)),
            Shape::Cmp => ordered.then(|| self.types.func(&[v], &[int], false)),
            Shape::Index => indexed(true).map(|elem| self.types.func(&[int], &[elem], false)),
            // A write through a value would change a copy.
            Shape::SetIndex => indexed(false).map(|elem| self.types.func(&[int, elem], &[], false)),
            Shape::Iter => None,
        };
        sigs.extend(given);
        (sigs, !own.is_empty())
    }

    /// The `@Operator` methods of the name that a value of the type has.
    /// A type parameter has the methods of the name that its constraint
    /// lists (GEN-13).
    pub(super) fn operator_methods(&self, ty: TypeId, name: &str) -> Vec<Operator> {
        if self.is_untyped(ty) {
            return Vec::new();
        }
        let Some(name) = self.names.get(name) else {
            return Vec::new();
        };
        match &*self.types.get(self.types.unalias(ty)) {
            Type::Named { .. } => self.types.operators(ty, name),
            Type::Param(_) => {
                let listed = self.types.param_set(ty);
                let method = listed.and_then(|set| set.methods.get((name, None)).cloned());
                let operator = method.map(|method| Operator {
                    name,
                    sig: method.sig,
                    marks: Marks::OPERATOR,
                    pointer_receiver: false,
                    holder: ty,
                    at: 0,
                });
                operator.into_iter().collect()
            }
            _ => Vec::new(),
        }
    }

    /// The one of the methods that takes the operands `args` (EXP-17):
    /// the one whose parameters have their types, a constant without a
    /// type counting as one of its default type. With `weak`, where none
    /// does, the only one whose parameters can hold the constants. `Err`
    /// if several can.
    fn pick(
        &mut self,
        methods: &[Operator],
        args: &[Operand],
        weak: bool,
    ) -> Result<Option<usize>, ()> {
        self.steps += methods.len() as u64;
        let exact = |this: &Self, arg: &Operand, param: TypeId| {
            arg.ty != Types::INVALID && this.types.identical(this.types.default_type(arg.ty), param)
        };
        let loose = |this: &Self, arg: &Operand, param: TypeId| match this.is_untyped(arg.ty) {
            true => this.implicit(arg, param).is_ok(),
            false => this.types.identical(arg.ty, param),
        };
        let takes =
            |this: &Self, method: &Operator, fits: &dyn Fn(&Self, &Operand, TypeId) -> bool| {
                let (params, ..) = this.func_parts(method.sig);
                params.len() == args.len()
                    && args
                        .iter()
                        .zip(&params)
                        .all(|(arg, &param)| fits(this, arg, param))
            };
        if let Some(at) = methods
            .iter()
            .position(|method| takes(self, method, &exact))
        {
            return Ok(Some(at));
        }
        if !weak || !args.iter().any(|arg| self.is_untyped(arg.ty)) {
            return Ok(None);
        }
        let mut fitting = (0..methods.len()).filter(|&at| takes(self, &methods[at], &loose));
        match (fitting.next(), fitting.next()) {
            (Some(_), Some(_)) => Err(()),
            (only, _) => Ok(only),
        }
    }

    /// The method `method` stands for an expression at `at`: its
    /// operands `args` get the types of its parameters.
    fn operator_used(
        &mut self,
        cx: &Cx<'a>,
        at: Span,
        method: &Operator,
        args: &[(ExprId, Operand)],
        swapped: bool,
    ) -> OperatorUse {
        let (params, ..) = self.func_parts(method.sig);
        for ((expr, x), &param) in args.iter().zip(&params) {
            let mut x = x.clone();
            self.convert_untyped(cx, *expr, &mut x, param, true);
        }
        self.touch_operator(method.holder, method.name, at.lo);
        let declared = match &*self.types.get(self.types.unalias(method.holder)) {
            Type::Named { id, .. } => Some(*id),
            _ => None,
        };
        if declared.is_some() && !method.marks.has(Marks::CONST) {
            let what = format!(
                "uses the operator method {}.{}, which is not @Const",
                self.type_string(method.holder),
                self.names.resolve(&method.name)
            );
            self.impure(at, &what);
        }
        // What is being declared needs the method.
        let node = declared
            .and_then(|id| self.operator_spans.get(&(id, method.at)))
            .and_then(|span| self.method_nodes.get(span))
            .copied();
        if let Some(node) = node {
            self.need(node);
        }
        OperatorUse {
            holder: method.holder,
            name: method.name,
            method: declared.map(|_| method.at),
            sig: method.sig,
            swapped,
        }
    }

    /// Whether Go's operator `op` takes the two operands as they are.
    fn builtin_binary(&self, op: BinaryOp, x: &Operand, y: &Operand) -> bool {
        if matches!(op, BinaryOp::Shl | BinaryOp::Shr) {
            let count = self.is_untyped(y.ty) || self.all_integer(y.ty);
            return count && (self.is_untyped(x.ty) || self.all_integer(x.ty));
        }
        // An operand without a type takes the type of the other.
        let (left, right) = match (self.is_untyped(x.ty), self.is_untyped(y.ty)) {
            (true, false) if self.implicit(x, y.ty).is_ok() => (y.ty, y.ty),
            (false, true) if self.implicit(y, x.ty).is_ok() => (x.ty, x.ty),
            _ => (x.ty, y.ty),
        };
        let interface = |ty: TypeId| !self.is_param(ty) && self.types.is_interface(ty);
        match op {
            BinaryOp::Eq | BinaryOp::Ne => {
                // A value is compared with what an interface holds.
                let mixed = (interface(left) || interface(right))
                    && (!self.not_assignable(x, right) || !self.not_assignable(y, left));
                mixed || (self.types.identical(left, right) && self.types.comparable(left))
            }
            _ if !self.types.identical(left, right) => false,
            BinaryOp::Lt | BinaryOp::Le | BinaryOp::Gt | BinaryOp::Ge => self
                .all_types(left, |this, ty| {
                    this.is_string(ty) || (this.is_numeric(ty) && !this.is_complex(ty))
                }),
            BinaryOp::Add => {
                self.all_types(left, |this, ty| this.is_numeric(ty) || this.is_string(ty))
            }
            BinaryOp::Sub | BinaryOp::Mul | BinaryOp::Div => self.all_numeric(left),
            BinaryOp::AndAnd | BinaryOp::OrOr => self.all_boolean(left),
            _ => self.all_integer(left),
        }
    }

    /// The method that stands for `x op y` in a `.gopp` file, if the
    /// types of the operands have one, and whether its receiver is `y`
    /// (EXP-17, EXP-20, EXP-26). `written` is the expression and `at`
    /// where it is, for what is said of it.
    fn operator_choice(
        &mut self,
        op: BinaryOp,
        (x, y): (&Operand, &Operand),
        (at, written): (Span, &str),
    ) -> Choice {
        let Some(name) = binary_method(op) else {
            return Choice::Builtin;
        };
        if x.ty == Types::INVALID || y.ty == Types::INVALID {
            return Choice::Builtin;
        }
        // `nil` is compared as Go compares it.
        let nil = self.types.basic(Basic::UntypedNil);
        if x.ty == nil || y.ty == nil {
            return Choice::Builtin;
        }
        let both_orders = COMMUTATIVE.contains(&name);
        let all_left = self.operator_methods(x.ty, name);
        let all_right = self.operator_methods(y.ty, name);
        if all_left.is_empty() && all_right.is_empty() {
            return Choice::Builtin;
        }
        let builtin = self.builtin_binary(op, x, y);
        let is_right = |method: &Operator| method.marks.has(Marks::RIGHT);
        // A method without `@Right` has its receiver on the left; one
        // with it has its receiver on the right, and on either side if
        // the operands may change places.
        let of_left: Vec<Operator> = all_left
            .iter()
            .filter(|method| !is_right(method) || both_orders)
            .cloned()
            .collect();
        let of_right: Vec<Operator> = all_right.iter().filter(|m| is_right(m)).cloned().collect();
        // A comparison that a type inherits hides the one that Go gives
        // the type (EXP-30).
        let hidden = matches!(name, "Eq" | "Cmp")
            && self.types.identical(x.ty, y.ty)
            && !all_left.is_empty()
            && all_left
                .iter()
                .all(|method| !self.types.identical(method.holder, x.ty));
        let from_left = self.pick(&of_left, std::slice::from_ref(y), !builtin);
        let from_right = self.pick(&of_right, std::slice::from_ref(x), !builtin);
        match (from_left, from_right) {
            (Ok(Some(at)), Ok(None)) => Choice::Method(of_left[at].clone(), false),
            (Ok(None), Ok(Some(at))) => Choice::Method(of_right[at].clone(), true),
            (Ok(None), Ok(None)) if builtin && !hidden => Choice::Builtin,
            (Ok(None), Ok(None)) => {
                self.no_operator((at, written), op, (x, y));
                Choice::Refused
            }
            _ => {
                let message = format!(
                    "invalid operation: {written} (more than one @Operator {name} method takes \
                     these operands)"
                );
                self.error(Code::OpAmbiguous, at, message);
                Choice::Refused
            }
        }
    }

    /// `left op right`, or `left op= right` without `whole`, in a `.gopp`
    /// file: what the method that stands for it gives.
    pub(super) fn operator_binary(
        &mut self,
        cx: &Cx<'a>,
        whole: Option<ExprId>,
        op: BinaryOp,
        (left, x): (ExprId, &Operand),
        (right, y): (ExprId, &Operand),
    ) -> Overload {
        let written = match whole {
            Some(expr) => self.expr_string(cx, expr),
            None => format!(
                "{} {}= {}",
                self.expr_string(cx, left),
                binary_text(op),
                self.expr_string(cx, right)
            ),
        };
        let at = self.span(cx, whole.unwrap_or(left));
        let (method, swapped) = match self.operator_choice(op, (x, y), (at, &written)) {
            Choice::Builtin => return Overload::Builtin,
            Choice::Refused => return Overload::Refused,
            Choice::Method(method, swapped) => (method, swapped),
        };
        let (receiver, arg) = match swapped {
            false => (left, (right, y.clone())),
            true => (right, (left, x.clone())),
        };
        let used = self.operator_used(cx, at, &method, &[arg], swapped);
        // The other operand has its type for good.
        let own = if swapped { y } else { x };
        self.update_expr_type(cx, receiver, own.ty, true);
        let (_, results, _) = self.func_parts(method.sig);
        match whole {
            Some(whole) => self.operators.insert(whole, used),
            // `x op= y` is `x = x op y`: the place takes what the method
            // gives.
            None => {
                let result = Operand::value(results[0]);
                if self.not_assignable(&result, x.ty) {
                    let message = format!(
                        "invalid operation: {written} (@Operator {} gives a {}, which is no \
                         value of the type {} of {})",
                        binary_method(op).unwrap_or_default(),
                        self.type_string(results[0]),
                        self.type_string(x.ty),
                        self.expr_string(cx, left)
                    );
                    self.error(Code::OpNoMethod, at, message);
                    return Overload::Refused;
                }
                self.op_assigns.insert(left, used)
            }
        };
        let result = match op {
            BinaryOp::Eq
            | BinaryOp::Ne
            | BinaryOp::Lt
            | BinaryOp::Le
            | BinaryOp::Gt
            | BinaryOp::Ge => self.value_of(Basic::UntypedBool),
            _ => Operand::value(results[0]),
        };
        // The receiver is the first operand of the method.
        let operands = match swapped {
            false => [x, y],
            true => [y, x],
        };
        let at = (whole.unwrap_or(left), at);
        Overload::Chosen(self.constant_operator(cx, at, &method, &operands, result))
    }

    /// The value `case` of a clause of a switch on `tag` is compared
    /// with it as `tag == case` is: whether an `@Operator` `Eq` method
    /// does it, or the case is refused for want of one.
    pub(super) fn operator_case(
        &mut self,
        cx: &Cx<'a>,
        (tag, x): (ExprId, &Operand),
        (case, v): (ExprId, &Operand),
    ) -> bool {
        let written = format!(
            "{} == {}",
            self.expr_string(cx, tag),
            self.expr_string(cx, case)
        );
        let at = self.span(cx, case);
        match self.operator_choice(BinaryOp::Eq, (x, v), (at, &written)) {
            Choice::Builtin => false,
            Choice::Refused => true,
            Choice::Method(method, swapped) => {
                let arg = match swapped {
                    false => (case, v.clone()),
                    true => (tag, x.clone()),
                };
                let used = self.operator_used(cx, at, &method, &[arg], swapped);
                self.case_operators.insert(case, used);
                true
            }
        }
    }

    /// `x[indices]` in a `.gopp` file, where the type of `x` has an
    /// `@Operator` method for it: `Index` gives the value, which is no
    /// place; `SetIndex` takes the value that is assigned (EXP-22).
    pub(super) fn operator_index(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        of: ExprId,
        x: &Operand,
        indices: &[ExprId],
    ) -> Option<Operand> {
        let store = self.lookup_target == Some(expr);
        let reading = self.operator_methods(x.ty, "Index");
        let storing = self.operator_methods(x.ty, "SetIndex");
        if reading.is_empty() && storing.is_empty() {
            return None;
        }
        let args: Vec<(ExprId, Operand)> = indices
            .iter()
            .map(|&index| (index, self.value(cx, index)))
            .collect();
        if args.iter().any(|(_, arg)| arg.mode == Mode::Unknown) {
            return Some(Operand::UNKNOWN);
        }
        let (name, methods) = match store {
            true => ("SetIndex", storing),
            false => ("Index", reading),
        };
        let at = self.span(cx, expr);
        let written = self.expr_string(cx, expr);
        let shown = self.type_string(x.ty);
        // `SetIndex` is picked by its indexes: its last parameter takes
        // the value.
        let by_indexes: Vec<Operator> = methods
            .iter()
            .map(|method| {
                let (params, ..) = self.func_parts(method.sig);
                let indexes = match store {
                    true => &params[..params.len() - 1],
                    false => &params[..],
                };
                Operator {
                    sig: self.types.func(indexes, &[], false),
                    ..method.clone()
                }
            })
            .collect();
        let operands: Vec<Operand> = args.iter().map(|(_, arg)| arg.clone()).collect();
        let method = match self.pick(&by_indexes, &operands, true) {
            Ok(Some(at)) => methods[at].clone(),
            Ok(None) => {
                let types: Vec<String> = operands
                    .iter()
                    .map(|arg| self.type_string(self.types.default_type(arg.ty)))
                    .collect();
                let message = format!(
                    "invalid operation: {written} ({shown} has no @Operator {name} method for \
                     {})",
                    types.join(", ")
                );
                self.error(Code::OpNoMethod, at, message);
                return Some(Operand::UNKNOWN);
            }
            Err(()) => {
                let message = format!(
                    "invalid operation: {written} (more than one @Operator {name} method takes \
                     these indexes)"
                );
                self.error(Code::OpAmbiguous, at, message);
                return Some(Operand::UNKNOWN);
            }
        };
        // A method with a pointer receiver works on the place itself.
        if method.pointer_receiver && x.mode != Mode::Variable {
            let message = format!(
                "invalid operation: {written} (@Operator {name} has a pointer receiver, and {} \
                 is no place)",
                self.expr_string(cx, of)
            );
            self.error(Code::OpNotAddressable, at, message);
            return Some(Operand::UNKNOWN);
        }
        let used = self.operator_used(cx, at, &method, &args, false);
        let (params, results, _) = self.func_parts(method.sig);
        Some(match store {
            true => {
                self.index_stores.insert(expr, used);
                Operand {
                    mode: Mode::MapIndex,
                    ty: params[params.len() - 1],
                }
            }
            false => {
                self.operators.insert(expr, used);
                Operand::value(results[0])
            }
        })
    }

    /// What a `range` clause ranges over in a `.gopp` file: for a value
    /// whose type has an `@Operator` `Iter` method, what the method
    /// gives (RNG-6b).
    pub(super) fn operator_iter(&mut self, cx: &Cx<'a>, of: ExprId, x: Operand) -> Operand {
        let methods = self.operator_methods(x.ty, "Iter");
        let Some(method) = methods.first() else {
            return x;
        };
        if method.pointer_receiver && x.mode != Mode::Variable {
            let message = format!(
                "cannot range over {}: @Operator Iter has a pointer receiver, and it is no place",
                self.describe(cx, of, &x)
            );
            self.error(Code::OpNotAddressable, self.span(cx, of), message);
            return Operand::UNKNOWN;
        }
        // Apart from the operators: what is ranged over may be an
        // expression that a method stands for itself.
        let used = self.operator_used(cx, self.span(cx, of), method, &[], false);
        self.range_operators.insert(of, used);
        let (_, results, _) = self.func_parts(method.sig);
        Operand::value(results[0])
    }

    /// `T(x)` in a `.gopp` file: the `@Convert` method of the type of
    /// `x` that gives a `T`, if it has one (EXP-25).
    pub(super) fn operator_convert(&mut self, call: ExprId, x: &Operand, target: TypeId) -> bool {
        let Some(method) = self.convert_method(x.ty, target) else {
            return false;
        };
        // Where it is written is not known here: it counts as late.
        self.touch_operator(method.holder, method.name, u32::MAX);
        self.operators.insert(call, method);
        true
    }

    /// The `@Convert` method of the type `from` that gives a `to`.
    pub(super) fn convert_method(&self, from: TypeId, to: TypeId) -> Option<OperatorUse> {
        if self.is_untyped(from) {
            return None;
        }
        let methods = self.types.marked_methods(from, Marks::CONVERT);
        let gives = |sig: TypeId| {
            let (_, results, _) = self.func_parts(sig);
            matches!(results[..], [result] if self.types.identical(result, to))
        };
        let (method, holder) = methods.into_iter().find(|(method, _)| gives(method.sig))?;
        Some(OperatorUse {
            holder,
            name: method.name,
            method: None,
            sig: method.sig,
            swapped: false,
        })
    }

    /// `left op= right` in a `.gopp` file: the method that does it in
    /// place, if the type of the place has one for the operand (EXP-19).
    pub(super) fn operator_in_place(
        &mut self,
        cx: &Cx<'a>,
        op: BinaryOp,
        (left, x): (ExprId, &Operand),
        (right, y): (ExprId, &Operand),
    ) -> Overload {
        let Some(binary) = binary_method(op) else {
            return Overload::Builtin;
        };
        // Only a place is changed where it is.
        if x.mode != Mode::Variable || x.ty == Types::INVALID || y.ty == Types::INVALID {
            return Overload::Builtin;
        }
        let name = format!("{binary}Assign");
        let methods = self.operator_methods(x.ty, &name);
        if methods.is_empty() {
            return Overload::Builtin;
        }
        let builtin = self.builtin_binary(op, x, y);
        let (place, value) = (self.expr_string(cx, left), self.expr_string(cx, right));
        let symbol = binary_text(op);
        let at = self.span(cx, left);
        let method = match self.pick(&methods, std::slice::from_ref(y), !builtin) {
            Ok(Some(at)) => methods[at].clone(),
            Ok(None) => return Overload::Builtin,
            Err(()) => {
                let message = format!(
                    "invalid operation: {place} {symbol}= {value} (more than one @Operator \
                     {name} method takes this operand)"
                );
                self.error(Code::OpAmbiguous, at, message);
                return Overload::Refused;
            }
        };
        // The method writes its receiver while it reads its operand.
        let inner = self.unparen(cx, right);
        let part = |of: &str, whole: &str| {
            of.strip_prefix(whole)
                .is_some_and(|rest| rest.is_empty() || rest.starts_with(['.', '[']))
        };
        if self.is_plain_place(cx, inner) && (part(&value, &place) || part(&place, &value)) {
            let message = format!(
                "invalid operation: {place} {symbol}= {value} (@Operator {name} changes {place} \
                 while it reads {value})"
            );
            let fix = (
                Code::NoteOpFix,
                format!("write {place} = {place} {symbol} {value}"),
            );
            self.error_with_fix(Code::OpCompoundOverlap, at, message, fix);
            return Overload::Refused;
        }
        let used = self.operator_used(cx, at, &method, &[(right, y.clone())], false);
        self.op_assigns.insert(left, used);
        Overload::Chosen(Operand::value(x.ty))
    }

    /// Says that the type of the left operand has no method for the
    /// operator with the type of the right one.
    pub(super) fn no_operator(
        &mut self,
        (at, written): (Span, &str),
        op: BinaryOp,
        (x, y): (&Operand, &Operand),
    ) {
        let name = binary_method(op).unwrap_or_default();
        let symbol = binary_text(op);
        // A constant is named by the type that it has as a value.
        let shown = |ty: TypeId| self.type_string(self.types.default_type(ty));
        let (left, right) = (shown(x.ty), shown(y.ty));
        let of_left = self.operator_methods(x.ty, name);
        let of_right = self.operator_methods(y.ty, name);
        // Only the type on the right has the operator: its methods take
        // it as the left operand.
        if of_left.is_empty() && !of_right.is_empty() {
            let message = format!(
                "invalid operation: {written} (the @Operator {name} methods of {right} take a \
                 {right} as the left operand)"
            );
            let fix = format!("declare an @Operator @Right {name} method for the other order");
            let fix = (Code::NoteOpFix, fix);
            return self.error_with_fix(Code::OpNoMethod, at, message, fix);
        }
        let message = format!(
            "invalid operation: {written} ({left} has no operator {symbol} (@Operator {name}) \
             for {right})"
        );
        // An inherited comparison hides the one that Go gives the type.
        let inherited = matches!(name, "Eq" | "Cmp")
            && !of_left.is_empty()
            && self.types.identical(x.ty, y.ty)
            && of_left
                .iter()
                .all(|method| !self.types.identical(method.holder, x.ty));
        if inherited {
            let fix = format!("define @Operator {name} on {left}, or compare the parent parts");
            let fix = (Code::NoteOpFix, fix);
            return self.error_with_fix(Code::OpNoMethod, at, message, fix);
        }
        self.error(Code::OpNoMethod, at, message);
    }

    /// `op x` in a `.gopp` file: the method that stands for it, if the
    /// type of the operand has one.
    pub(super) fn operator_unary(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        op: UnaryOp,
        x: &Operand,
    ) -> Overload {
        let name = match op {
            UnaryOp::Neg => "Neg",
            UnaryOp::Not => "Not",
            UnaryOp::Xor => "BitNot",
            _ => return Overload::Builtin,
        };
        let methods = self.operator_methods(x.ty, name);
        let Some(method) = methods.first() else {
            return Overload::Builtin;
        };
        let used = self.operator_used(cx, self.span(cx, expr), method, &[], false);
        self.operators.insert(expr, used);
        let (_, results, _) = self.func_parts(method.sig);
        let result = Operand::value(results[0]);
        let at = (expr, self.span(cx, expr));
        Overload::Chosen(self.constant_operator(cx, at, method, &[x], result))
    }
}

/// An `@Operator` method as it was declared, to find one that is
/// declared again.
#[derive(Clone)]
pub(super) struct OperatorDecl {
    name: Spur,
    sig: TypeId,
    right: bool,
    span: Span,
}
