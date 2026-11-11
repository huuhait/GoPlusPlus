//! Declarations and the file.

use super::context::{GenDecl, Item, Spec};
use super::exprs::{ParamGroup, ParamMode, ParamType};
use super::imports;
use super::printer::{Printer, Ws};
use crate::ast::*;
use crate::token::TokenKind as T;

/// For each of the specifications of a `const` or `var` group, whether the
/// column of its type is kept. In a run of specifications with values
/// where none has a type, the values take the type column.
fn keep_type_column(has_type: &[bool], has_values: &[bool]) -> Vec<bool> {
    let mut keep = vec![false; has_type.len()];
    let mut run_start = None;
    let mut run_has_type = false;
    for index in 0..=has_type.len() {
        let in_run = has_values.get(index).copied().unwrap_or(false);
        match run_start {
            None if in_run => {
                run_start = Some(index);
                run_has_type = false;
            }
            Some(start) if !in_run => {
                if run_has_type {
                    keep[start..index].fill(true);
                }
                run_start = None;
            }
            _ => {}
        }
        run_has_type |= has_type.get(index).copied().unwrap_or(false);
    }
    keep
}

impl<'a> Printer<'a> {
    fn decl_node(&self, spec: Spec) -> &'a Decl {
        &self.cx.file.decl_nodes[spec.decl]
    }

    /// One specification of a `const` or `var` group of several.
    fn value_spec(&mut self, spec: Spec, keep_type: bool) {
        let cx = self.cx;
        let (DeclKind::Const { names, ty, values } | DeclKind::Var { names, ty, values }) =
            &self.decl_node(spec).kind
        else {
            return;
        };
        self.set_comment(cx.has_doc(spec.pos));
        self.ident_list(names, false);
        // How many columns a comment after the specification has to skip.
        let mut extra_tabs = 3;
        if ty.is_some() || keep_type {
            self.ws(Ws::Vtab);
            extra_tabs -= 1;
        }
        if let Some(ty) = *ty {
            self.expr(ty);
        }
        if !values.is_empty() {
            self.ws(Ws::Vtab);
            self.tok(T::Eq);
            self.ws(Ws::Blank);
            self.expr_list(None, values, 1, 0, None);
            extra_tabs -= 1;
        }
        if cx.line_comment(spec.end).is_some() {
            for _ in 0..extra_tabs {
                self.ws(Ws::Vtab);
            }
            self.set_comment(true);
        }
    }

    /// A specification that is alone, or in a group of `count` that has no
    /// columns of types and values.
    fn spec(&mut self, spec: Spec, count: usize, indent: bool) {
        let cx = self.cx;
        let decl = self.decl_node(spec);
        self.set_comment(cx.has_doc(spec.pos));
        match &decl.kind {
            &DeclKind::Import { name, path } => {
                let path_span = cx.file.exprs[path].span;
                if let Some(name) = name {
                    self.set_pos(if spec.moved { spec.pos } else { name.span.lo });
                    self.ident(cx.name(name));
                    self.ws(Ws::Blank);
                }
                self.set_pos(if spec.moved { spec.pos } else { path_span.lo });
                let text = cx.source(path_span.lo, path_span.hi);
                let canonical = imports::canonical_path(text);
                self.literal(T::String, canonical.as_deref().unwrap_or(text));
                if spec.moved {
                    self.set_pos(spec.end);
                }
            }
            DeclKind::Const { names, ty, values } | DeclKind::Var { names, ty, values } => {
                self.ident_list(names, indent);
                if let Some(ty) = *ty {
                    self.ws(Ws::Blank);
                    self.expr(ty);
                }
                if !values.is_empty() {
                    self.ws(Ws::Blank);
                    self.tok(T::Eq);
                    self.ws(Ws::Blank);
                    self.expr_list(None, values, 1, 0, None);
                }
            }
            DeclKind::Type {
                name,
                type_params,
                alias,
                ty,
            } => {
                self.name(*name);
                if let Some(last) = type_params.last() {
                    let open = cx.find(name.span.hi, T::LBrack);
                    let close = cx.lo(cx.closing(self.expr_end(last.bound)));
                    let groups = self.type_param_groups(type_params);
                    self.parameters(open, &groups, close, ParamMode::TypeTypeParams);
                }
                self.ws(if count == 1 { Ws::Blank } else { Ws::Vtab });
                if *alias {
                    self.tok(T::Eq);
                    self.ws(Ws::Blank);
                }
                self.expr(*ty);
            }
            _ => {}
        }
        self.set_comment(cx.line_comment(spec.end).is_some());
    }

    /// An attribute on a line of its own.
    fn attribute_line(&mut self, attr: &Attribute) {
        self.attribute(attr);
        self.ws(Ws::Newline);
        self.after_attribute = true;
    }

    pub(super) fn gen_decl(&mut self, decl: &GenDecl, attrs: &[Attribute]) {
        let cx = self.cx;
        for &(lo, hi) in &decl.orphans {
            self.set_pos(lo);
            self.literal(T::String, cx.source(lo, hi));
            self.ws(Ws::Newline);
        }
        for attr in attrs {
            self.attribute_line(attr);
        }
        self.set_pos(decl.pos);
        self.tok(decl.keyword);
        self.ws(Ws::Blank);

        let Some((lparen, rparen)) = decl.parens else {
            if let Some(&spec) = decl.specs.first() {
                self.spec(spec, 1, true);
            }
            return;
        };
        self.set_pos(lparen);
        self.tok(T::LParen);
        let count = decl.specs.len();
        if count > 0 {
            self.ws(Ws::Indent);
            self.ws(Ws::Formfeed);
            let keep_type = if count > 1 && matches!(decl.keyword, T::Const | T::Var) {
                let parts = |spec: &Spec| match &self.decl_node(*spec).kind {
                    DeclKind::Const { ty, values, .. } | DeclKind::Var { ty, values, .. } => {
                        (ty.is_some(), !values.is_empty())
                    }
                    _ => (false, false),
                };
                let (has_type, has_values): (Vec<bool>, Vec<bool>) =
                    decl.specs.iter().map(parts).unzip();
                Some(keep_type_column(&has_type, &has_values))
            } else {
                None
            };
            let slot = self.new_line_slot();
            for (index, &spec) in decl.specs.iter().enumerate() {
                if self.aborted {
                    break;
                }
                if index > 0 {
                    let new_section = self.lines_from(slot) > 0;
                    self.linebreak(cx.line(spec.pos), 1, Ws::Ignore, new_section);
                }
                self.record_line(slot);
                match &keep_type {
                    Some(keep_type) => self.value_spec(spec, keep_type[index]),
                    None => self.spec(spec, count, false),
                }
            }
            self.drop_line_slot(slot);
            self.ws(Ws::Unindent);
            self.ws(Ws::Formfeed);
        }
        self.set_pos(rparen);
        self.tok(T::RParen);
    }

    fn func_decl(&mut self, decl: &'a Decl) {
        let cx = self.cx;
        let DeclKind::Func {
            is_macro,
            receiver,
            name,
            type_params,
            ty,
            body,
        } = &decl.kind
        else {
            return;
        };
        for attr in &decl.attrs {
            self.attribute_line(attr);
        }
        self.set_pos(decl.span.lo);
        let mut func = cx.token_at(decl.span.lo);
        let mut header = "func ".len() as i32;
        if *is_macro {
            self.tok(T::Macro);
            self.ws(Ws::Blank);
            func += 1;
            header += "macro ".len() as i32;
            self.set_pos(cx.lo(func));
        }
        self.tok(T::Func);
        self.ws(Ws::Blank);
        // Only now is the white space before `func` written.
        let start_column = self.out.column - header;
        if let Some(receiver) = receiver {
            let groups = self.param_groups(std::slice::from_ref(receiver));
            let close = cx.lo(cx.closing(self.expr_end(receiver.ty)));
            self.parameters(cx.lo(func + 1), &groups, close, ParamMode::Func);
            self.ws(Ws::Blank);
        }
        self.name(*name);
        self.signature(name.span.hi, type_params, *ty);
        let header_size = self.distance_from(decl.span.lo, start_column);
        self.func_body(header_size, Ws::Vtab, body.as_ref());
    }

    /// `macro Name(a kind, b ...kind) { ... }`, laid out as a function.
    fn macro_decl(&mut self, decl: &'a Decl) {
        let cx = self.cx;
        let DeclKind::Macro { name, params, body } = &decl.kind else {
            return;
        };
        self.set_pos(decl.span.lo);
        self.tok(T::Macro);
        self.ws(Ws::Blank);
        let start_column = self.out.column - "macro ".len() as i32;
        self.name(*name);
        let groups: Vec<ParamGroup> = params
            .iter()
            .map(|param| ParamGroup {
                names: vec![param.name],
                ty: ParamType::Fragment {
                    variadic: param.variadic,
                    kind: param.kind,
                },
                pos: param.name.span.lo,
                end: param.kind.span.hi,
            })
            .collect();
        let open = cx.token_at(name.span.hi);
        let close = match params.last() {
            Some(last) => cx.closing(last.kind.span.hi),
            None => open + 1,
        };
        self.parameters(cx.lo(open), &groups, cx.lo(close), ParamMode::Func);
        let header_size = self.distance_from(decl.span.lo, start_column);
        self.func_body(header_size, Ws::Vtab, Some(body));
    }

    fn decl_list(&mut self) {
        let cx = self.cx;
        let mut keyword = None;
        for item in &cx.items {
            let (pos, end, attrs, kind): (u32, u32, &[Attribute], T) = match item {
                Item::Gen(decl) => {
                    let attrs = match decl.specs.first() {
                        Some(&first) => self.decl_node(first).attrs.as_slice(),
                        None => &[],
                    };
                    let end = match (decl.parens, decl.specs.last()) {
                        (Some((_, rparen)), _) => rparen + 1,
                        (None, Some(last)) => last.end,
                        (None, None) => decl.pos,
                    };
                    (decl.pos, end, attrs, decl.keyword)
                }
                Item::Other(id) => {
                    let decl = &cx.file.decl_nodes[*id];
                    let kind = match decl.kind {
                        DeclKind::Func {
                            is_macro: false, ..
                        } => T::Func,
                        DeclKind::MacroCall(_) => T::Hash,
                        _ => T::Macro,
                    };
                    (decl.span.lo, decl.span.hi, decl.attrs.as_slice(), kind)
                }
            };
            let orphan = match item {
                Item::Gen(decl) => decl.orphans.first().map(|&(lo, _)| lo),
                Item::Other(_) => None,
            };
            let start = attrs.first().map(|attr| attr.span.lo).or(orphan);
            let start = start.unwrap_or(pos);

            // A blank line goes between declarations of different kinds
            // and before a documented one. A function over several lines
            // shares no column with what is before it.
            let previous = keyword.replace(kind);
            let min = if previous != Some(kind) || cx.has_doc(start) {
                2
            } else {
                1
            };
            let lines = cx.line(end) - cx.line(start) + 1;
            let new_section = matches!(kind, T::Func | T::Macro) && lines > 1;
            self.linebreak(cx.line(start), min, Ws::Ignore, new_section);

            match item {
                Item::Gen(decl) => self.gen_decl(decl, attrs),
                Item::Other(id) => {
                    let decl = &cx.file.decl_nodes[*id];
                    match decl.kind {
                        DeclKind::Func { .. } => self.func_decl(decl),
                        DeclKind::Macro { .. } => self.macro_decl(decl),
                        DeclKind::MacroCall(call) => self.expr(call),
                        _ => {
                            self.set_pos(decl.span.lo);
                            self.literal(T::String, cx.source(decl.span.lo, decl.span.hi));
                        }
                    }
                }
            }
        }
    }

    pub(super) fn file(&mut self) {
        let cx = self.cx;
        if let Some(package) = cx.file.package {
            let keyword = cx.token_at(package.span.lo).saturating_sub(1);
            self.set_pos(cx.lo(keyword));
            self.tok(T::Package);
            self.ws(Ws::Blank);
            self.name(package);
        }
        self.decl_list();
        self.ws(Ws::Newline);
    }
}

#[cfg(test)]
mod tests {
    use super::keep_type_column;

    #[test]
    fn values_take_the_type_column_of_a_run_without_types() {
        let (t, f) = (true, false);
        // The specifications `a T = 1`, `b = 2`, `c`, `d = 3`.
        let keep = keep_type_column(&[t, f, f, f], &[t, t, f, t]);
        assert_eq!(keep, [t, t, f, f]);
    }
}
