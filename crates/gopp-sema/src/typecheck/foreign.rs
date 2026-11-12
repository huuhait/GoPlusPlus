//! The names of C and C++ headers (chapter 14): what a header declares
//! with a name is asked of what read the header, once for each name that
//! the package uses. `pkg.f[x]` gives a template its first arguments
//! (ADR-0010), and what a function does not change is known for each
//! parameter (ADR-0008).

use super::*;
use crate::foreign::{ForeignMember, ForeignPackage, TemplateArg};
use crate::object::ForeignUse;

/// A member of a header as an expression names it.
type Named<'a> = (ObjId, &'a dyn ForeignPackage, Ident);

impl<'a> Checker<'a> {
    /// The header and the name, if `expr` is `pkg.name` for a `pkg` that
    /// is a header.
    fn foreign_named(&self, cx: &Cx<'a>, expr: ExprId) -> std::option::Option<Named<'a>> {
        let ExprKind::Selector { x, sel } = cx.tree.exprs[self.unparen(cx, expr)].kind else {
            return None;
        };
        let Some(&Ref::Object(import)) = cx.body.uses.get(&x) else {
            return None;
        };
        let ObjKind::Import { package, .. } = &self.package.object(import).kind else {
            return None;
        };
        Some((import, package.foreign.as_deref()?, sel))
    }

    /// What the header declares with the name: asked once.
    fn foreign_member(
        &mut self,
        (import, hooks, name): Named<'a>,
    ) -> std::option::Option<ForeignMember> {
        if let Some(known) = self.foreign_members.get(&(import, name.name)) {
            return known.clone();
        }
        self.steps += 1;
        let text = self.names.resolve(&name.name);
        let member = hooks.member(text, self.types, self.names);
        self.foreign_members
            .insert((import, name.name), member.clone());
        member
    }

    /// Asks the headers for every name of theirs that the files use,
    /// before the bodies are checked side by side: what a header makes
    /// for a name is made once, in the table that all share.
    pub(super) fn foreign_names(&mut self, units: &[Unit<'a>]) {
        let package = self.package;
        let header = |import: ObjId| match &package.object(import).kind {
            ObjKind::Import { package, .. } => package.foreign.as_deref(),
            _ => None,
        };
        let any = (0..package.objects.len()).any(|at| header(ObjId(at as u32)).is_some());
        if !any {
            return;
        }
        for (file, unit) in units.iter().enumerate() {
            let mut named: FxHashMap<ExprId, ObjId> = FxHashMap::default();
            for body in &package.files[file].bodies {
                for (&expr, denotes) in &body.uses {
                    if let Ref::Object(import) = *denotes
                        && header(import).is_some()
                    {
                        named.insert(expr, import);
                    }
                }
            }
            for (_, node) in unit.file.exprs.iter() {
                self.steps += 1;
                if let ExprKind::Selector { x, sel } = node.kind
                    && let Some(&import) = named.get(&x)
                    && let Some(hooks) = header(import)
                {
                    self.foreign_member((import, hooks, sel));
                }
            }
        }
    }

    fn foreign_undefined(&mut self, cx: &Cx<'a>, expr: ExprId, name: Ident) {
        let message = format!("undefined: {}", self.expr_string(cx, expr));
        self.error(Code::Undefined, name.span, message);
    }

    /// `pkg.name` where a value is written, for a `pkg` that is a header.
    /// `None` if `pkg` is none.
    pub(super) fn foreign_selector(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
    ) -> std::option::Option<Operand> {
        let named = self.foreign_named(cx, expr)?;
        let (import, _, name) = named;
        let Some(member) = self.foreign_member(named) else {
            self.foreign_undefined(cx, expr, name);
            return Some(Operand::UNKNOWN);
        };
        let used = |ty: TypeId| ForeignUse {
            import,
            name: name.name,
            explicit: Vec::new(),
            ty,
        };
        Some(match member {
            ForeignMember::Func { sig, read_only } => {
                self.foreign_uses.insert(expr, used(sig));
                self.foreign_flags.insert(expr, read_only);
                Operand::value(sig)
            }
            ForeignMember::Var { ty, read_only } => {
                self.foreign_uses.insert(expr, used(ty));
                match read_only {
                    true => Operand::value(ty),
                    false => Operand::variable(ty),
                }
            }
            ForeignMember::Const { ty, value } => Operand::constant(ty, value),
            ForeignMember::Type(ty) => Operand {
                mode: Mode::Type,
                ty,
            },
            ForeignMember::Template => {
                let message = format!(
                    "cannot use {} without its template arguments: it is a template",
                    self.expr_string(cx, expr)
                );
                self.error(Code::ForeignTemplate, self.span(cx, expr), message);
                Operand::UNKNOWN
            }
        })
    }

    /// The arguments that are written for a template in brackets: types
    /// and constants.
    fn template_args(
        &mut self,
        cx: &Cx<'a>,
        indices: &[ExprId],
    ) -> std::option::Option<Vec<TemplateArg>> {
        let mut explicit = Vec::new();
        for &index in indices {
            let x = self.operand(cx, index);
            explicit.push(match x.mode {
                Mode::Unknown => return None,
                Mode::Type => TemplateArg::Type(x.ty),
                Mode::Const(value) => TemplateArg::Const(value, x.ty),
                _ => {
                    let message = format!(
                        "invalid template argument {}: it is no type and no constant",
                        self.describe(cx, index, &x)
                    );
                    self.error(Code::ForeignTemplate, self.span(cx, index), message);
                    return None;
                }
            });
        }
        Some(explicit)
    }

    /// `pkg.name[indices]` where a value is written and not called, for
    /// a `pkg` that is a header: the instance of a template. `None` where
    /// the brackets are an index.
    pub(super) fn foreign_index(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        of: ExprId,
        indices: &[ExprId],
    ) -> std::option::Option<Operand> {
        let named = self.foreign_named(cx, of)?;
        let (import, hooks, name) = named;
        // A variable is indexed; with brackets, a function is a template.
        let is_template = matches!(
            self.foreign_member(named)?,
            ForeignMember::Template | ForeignMember::Func { .. }
        );
        if !is_template {
            return None;
        }
        let Some(explicit) = self.template_args(cx, indices) else {
            return Some(Operand::UNKNOWN);
        };
        let at = self.span(cx, expr);
        let written = self.expr_string(cx, expr);
        let text = self.names.resolve(&name.name);
        let instance = hooks.instance(text, &explicit, None, self.types, self.names);
        self.steps += 1;
        let used = |ty: TypeId| ForeignUse {
            import,
            name: name.name,
            explicit: explicit.clone(),
            ty,
        };
        Some(match instance {
            Ok(ForeignMember::Var { ty, read_only }) => {
                self.foreign_uses.insert(expr, used(ty));
                match read_only {
                    true => Operand::value(ty),
                    false => Operand::variable(ty),
                }
            }
            Ok(ForeignMember::Const { ty, value }) => Operand::constant(ty, value),
            Ok(ForeignMember::Type(ty)) => Operand {
                mode: Mode::Type,
                ty,
            },
            // An instance of a function template is only called.
            Ok(ForeignMember::Func { .. }) => {
                let message = format!(
                    "cannot use {written} as a value: an instance of a function template is \
                     only called"
                );
                let fix = (Code::NoteOpFix, "call it in a function literal".to_owned());
                self.error_with_fix(Code::ForeignTemplate, at, message, fix);
                Operand::UNKNOWN
            }
            Ok(ForeignMember::Template) => {
                let message = format!("cannot use {written}: it needs more template arguments");
                self.error(Code::ForeignTemplate, at, message);
                Operand::UNKNOWN
            }
            Err(reason) => {
                let message = format!("cannot use {written}: {reason}");
                self.error(Code::ForeignTemplate, at, message);
                Operand::UNKNOWN
            }
        })
    }

    /// What the call of `fun` with the arguments `args` calls, if `fun`
    /// names a template of a header: the instance for the template
    /// arguments in brackets and those that the arguments give. `None`
    /// if `fun` is no template.
    pub(super) fn foreign_callee(
        &mut self,
        cx: &Cx<'a>,
        fun: ExprId,
        args: &[ExprId],
    ) -> std::option::Option<Operand> {
        let fun = self.unparen(cx, fun);
        let (of, indices) = match &cx.tree.exprs[fun].kind {
            ExprKind::Index { x, indices } => (*x, &indices[..]),
            _ => (fun, &[][..]),
        };
        let named = self.foreign_named(cx, of)?;
        let (import, hooks, name) = named;
        // With brackets, only templates are called.
        match self.foreign_member(named)? {
            ForeignMember::Template => {}
            ForeignMember::Func { .. } if !indices.is_empty() => {}
            _ => return None,
        }
        let explicit = self.template_args(cx, indices);
        let mut given = Vec::new();
        for &arg in args {
            let x = self.value(cx, arg);
            if x.mode == Mode::Unknown {
                return Some(Operand::UNKNOWN);
            }
            given.push(x.ty);
        }
        let Some(explicit) = explicit else {
            return Some(Operand::UNKNOWN);
        };
        let at = self.span(cx, fun);
        let written = self.expr_string(cx, fun);
        let text = self.names.resolve(&name.name);
        let instance = hooks.instance(text, &explicit, Some(&given), self.types, self.names);
        self.steps += 1;
        Some(match instance {
            Ok(ForeignMember::Func { sig, read_only }) => {
                let used = ForeignUse {
                    import,
                    name: name.name,
                    explicit,
                    ty: sig,
                };
                self.foreign_uses.insert(fun, used);
                self.foreign_flags.insert(fun, read_only);
                Operand::value(sig)
            }
            // A class template with its arguments converts.
            Ok(ForeignMember::Type(ty)) => Operand {
                mode: Mode::Type,
                ty,
            },
            Ok(_) => {
                let message = format!("cannot call {written}: it is no function");
                self.error(Code::ForeignTemplate, at, message);
                Operand::UNKNOWN
            }
            Err(reason) => {
                let message = format!("cannot call {written}: {reason}");
                self.error(Code::ForeignTemplate, at, message);
                Operand::UNKNOWN
            }
        })
    }

    /// A function of a header is called with `args`: what a `Shared`
    /// holds is only read, so a pointer into it is given only where the
    /// function does not write through it (ADR-0008).
    pub(super) fn foreign_arguments(&mut self, cx: &Cx<'a>, fun: ExprId, args: &[ExprId]) {
        let fun = self.unparen(cx, fun);
        let Some(read_only) = self.foreign_flags.get(&fun).cloned() else {
            return;
        };
        for (&arg, flag) in args.iter().zip(read_only) {
            if flag || !self.points_into_shared(cx, arg) {
                continue;
            }
            let message = format!(
                "cannot give {} to {}: the function may write through it, and what a Shared \
                 holds is only read",
                self.expr_string(cx, arg),
                self.expr_string(cx, fun)
            );
            self.error(Code::ForeignReadOnly, self.span(cx, arg), message);
        }
    }

    /// `pkg.name`, or `pkg.name[indices]`, where a type is written, for
    /// a `pkg` that is a header. `None` if `pkg` is none.
    pub(super) fn foreign_type(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        of: ExprId,
        indices: &[ExprId],
    ) -> std::option::Option<TypeId> {
        let named = self.foreign_named(cx, of)?;
        let (_, hooks, name) = named;
        let Some(member) = self.foreign_member(named) else {
            self.foreign_undefined(cx, of, name);
            return Some(Types::INVALID);
        };
        let at = self.span(cx, expr);
        let written = self.expr_string(cx, expr);
        let member = match (member, indices.is_empty()) {
            (member, true) => Ok(member),
            (ForeignMember::Template, false) => {
                let explicit = self.template_args(cx, indices)?;
                let text = self.names.resolve(&name.name);
                self.steps += 1;
                hooks.instance(text, &explicit, None, self.types, self.names)
            }
            (_, false) => Err("it is no template".to_owned()),
        };
        Some(match member {
            Ok(ForeignMember::Type(ty)) => ty,
            Ok(ForeignMember::Template) => {
                let message = format!("cannot use {written} without its template arguments");
                self.error(Code::ForeignTemplate, at, message);
                Types::INVALID
            }
            Ok(_) => {
                self.error(Code::NotAType, at, format!("{written} is not a type"));
                Types::INVALID
            }
            Err(reason) => {
                let message = format!("cannot use {written}: {reason}");
                self.error(Code::ForeignTemplate, at, message);
                Types::INVALID
            }
        })
    }
}
