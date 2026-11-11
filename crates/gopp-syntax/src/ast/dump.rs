//! A text form of the tree for snapshots and `-emit=ast`.

use std::fmt::Write;

use gopp_span::Span;
use lasso::ThreadedRodeo;

use super::*;
use crate::with_stack;

const MAX_INDENT: usize = 100;

/// One line per node, children indented, each with its byte range.
pub fn dump(file: &File, text: &str, names: &ThreadedRodeo) -> String {
    let mut dumper = Dumper {
        file,
        text,
        names,
        out: String::new(),
        depth: 0,
    };
    let package = file.package.map_or("?", |ident| names.resolve(&ident.name));
    dumper.line(format_args!("File package={package}"), None);
    dumper.nested(|d| file.decls.iter().for_each(|&decl| d.decl(decl)));
    dumper.out
}

struct Dumper<'a> {
    file: &'a File,
    text: &'a str,
    names: &'a ThreadedRodeo,
    out: String,
    depth: usize,
}

impl<'a> Dumper<'a> {
    fn line(&mut self, text: impl std::fmt::Display, span: Option<Span>) {
        // Past the deepest indentation a line says how much deeper it is,
        // so that a long chain of nodes does not make the text quadratic.
        let indented = self.depth.min(MAX_INDENT);
        self.out.extend(std::iter::repeat_n("  ", indented));
        // Writing to a String cannot fail.
        if self.depth > MAX_INDENT {
            let _ = write!(self.out, "[+{}] ", self.depth - MAX_INDENT);
        }
        let _ = write!(self.out, "{text}");
        if let Some(span) = span {
            let _ = write!(self.out, " @{}..{}", span.lo, span.hi);
        }
        self.out.push('\n');
    }

    fn nested(&mut self, body: impl FnOnce(&mut Self)) {
        self.depth += 1;
        body(self);
        self.depth -= 1;
    }

    /// A named group of children.
    fn field(&mut self, name: &str, body: impl FnOnce(&mut Self)) {
        self.line(format_args!("{name}:"), None);
        self.nested(body);
    }

    fn name(&self, ident: Ident) -> &'a str {
        self.names.resolve(&ident.name)
    }

    fn names_of(&self, idents: &[Ident]) -> String {
        let names: Vec<&str> = idents.iter().map(|&ident| self.name(ident)).collect();
        names.join(",")
    }

    fn source(&self, span: Span) -> &'a str {
        &self.text[span.lo as usize..span.hi as usize]
    }

    fn macro_ref(&self, path: &MacroRef) -> String {
        match path.package {
            Some(package) => format!("{}.{}", self.name(package), self.name(path.name)),
            None => self.name(path.name).to_owned(),
        }
    }

    fn exprs(&mut self, name: &str, exprs: &[ExprId]) {
        if !exprs.is_empty() {
            self.field(name, |d| exprs.iter().for_each(|&expr| d.expr(expr)));
        }
    }

    fn opt_expr(&mut self, name: &str, expr: Option<ExprId>) {
        if let Some(expr) = expr {
            self.field(name, |d| d.expr(expr));
        }
    }

    fn opt_stmt(&mut self, name: &str, stmt: Option<StmtId>) {
        if let Some(stmt) = stmt {
            self.field(name, |d| d.stmt(stmt));
        }
    }

    fn stmts(&mut self, stmts: &[StmtId]) {
        stmts.iter().for_each(|&stmt| self.stmt(stmt));
    }

    fn block(&mut self, block: &Block) {
        self.line("Block", Some(block.span));
        self.nested(|d| d.stmts(&block.stmts));
    }

    fn attrs(&mut self, attrs: &[Attribute]) {
        for attr in attrs {
            let parens = if attr.args.is_some() { "()" } else { "" };
            let path = self.macro_ref(&attr.path);
            self.line(format_args!("Attribute {path}{parens}"), Some(attr.span));
            if let Some(args) = &attr.args {
                self.nested(|d| args.iter().for_each(|&arg| d.expr(arg)));
            }
        }
    }

    fn type_params(&mut self, params: &[TypeParam]) {
        for param in params {
            let extends = if param.extends { " extends" } else { "" };
            let name = self.name(param.name);
            self.line(
                format_args!("TypeParam {name}{extends}"),
                Some(param.name.span),
            );
            self.nested(|d| d.expr(param.bound));
        }
    }

    fn params(&mut self, kind: &str, params: &[Param]) {
        for param in params {
            let name = param
                .name
                .map_or(String::new(), |name| format!(" {}", self.name(name)));
            self.line(format_args!("{kind}{name}"), None);
            self.nested(|d| d.expr(param.ty));
        }
    }

    fn decl(&mut self, id: DeclId) {
        let file = self.file;
        let decl = &file.decl_nodes[id];
        let group = decl
            .group
            .map_or(String::new(), |group| format!(" group={group}"));
        let span = Some(decl.span);
        self.attrs(&decl.attrs);
        match &decl.kind {
            DeclKind::Bad => self.line("BadDecl", span),
            DeclKind::Import { name, path } => {
                let name = name.map_or(String::new(), |name| format!(" {}", self.name(name)));
                self.line(format_args!("Import{name}{group}"), span);
                self.nested(|d| d.expr(*path));
            }
            DeclKind::Const { names, ty, values } | DeclKind::Var { names, ty, values } => {
                let kind = if matches!(decl.kind, DeclKind::Const { .. }) {
                    "Const"
                } else {
                    "Var"
                };
                let names = self.names_of(names);
                self.line(format_args!("{kind} {names}{group}"), span);
                self.nested(|d| {
                    d.opt_expr("type", *ty);
                    d.exprs("values", values);
                });
            }
            DeclKind::Type {
                name,
                type_params,
                alias,
                extends,
                ty,
            } => {
                let alias = if *alias { " alias" } else { "" };
                let name = self.name(*name);
                self.line(format_args!("Type {name}{alias}{group}"), span);
                self.nested(|d| {
                    d.type_params(type_params);
                    d.exprs("extends", extends);
                    d.expr(*ty);
                });
            }
            DeclKind::Func {
                is_macro,
                receiver,
                name,
                type_params,
                ty,
                body,
            } => {
                let kind = if *is_macro { "MacroFunc" } else { "Func" };
                let name = self.name(*name);
                self.line(format_args!("{kind} {name}"), span);
                self.nested(|d| {
                    d.params("Receiver", receiver.as_slice());
                    d.type_params(type_params);
                    d.expr(*ty);
                    if let Some(body) = body {
                        d.block(body);
                    }
                });
            }
            DeclKind::Macro { name, params, body } => {
                let name = self.name(*name);
                self.line(format_args!("Macro {name}"), span);
                self.nested(|d| {
                    for param in params {
                        let dots = if param.variadic { "..." } else { "" };
                        let (name, kind) = (d.name(param.name), d.name(param.kind));
                        let line = format!("MacroParam {name} {dots}{kind}");
                        d.line(line, Some(param.name.span));
                    }
                    d.block(body);
                });
            }
            DeclKind::MacroCall(call) => {
                self.line("MacroCallDecl", span);
                self.nested(|d| d.expr(*call));
            }
        }
    }

    fn expr(&mut self, id: ExprId) {
        with_stack(|| self.expr_node(id));
    }

    fn expr_node(&mut self, id: ExprId) {
        let file = self.file;
        let expr = &file.exprs[id];
        let span = Some(expr.span);
        match &expr.kind {
            ExprKind::Bad => self.line("BadExpr", span),
            ExprKind::Name(name) => {
                let name = self.names.resolve(name);
                self.line(format_args!("Name {name}"), span);
            }
            ExprKind::BasicLit { kind, bad } => {
                let bad = if *bad { " bad" } else { "" };
                let text = self.source(expr.span);
                self.line(format_args!("BasicLit {kind:?}{bad} {text:?}"), span);
            }
            ExprKind::CompositeLit { ty, elems } => {
                self.line("CompositeLit", span);
                self.nested(|d| {
                    d.opt_expr("type", *ty);
                    elems.iter().for_each(|&elem| d.expr(elem));
                });
            }
            ExprKind::KeyValue { key, value } => {
                self.line("KeyValue", span);
                self.nested(|d| {
                    d.expr(*key);
                    d.expr(*value);
                });
            }
            ExprKind::FuncLit { ty, body } => {
                self.line("FuncLit", span);
                self.nested(|d| {
                    d.expr(*ty);
                    d.block(body);
                });
            }
            ExprKind::Paren(x) => {
                self.line("Paren", span);
                self.nested(|d| d.expr(*x));
            }
            ExprKind::Selector { x, sel } => {
                let sel = self.name(*sel);
                self.line(format_args!("Selector {sel}"), span);
                self.nested(|d| d.expr(*x));
            }
            ExprKind::Index { x, indices } => {
                self.line("Index", span);
                self.nested(|d| {
                    d.expr(*x);
                    d.exprs("indices", indices);
                });
            }
            ExprKind::Slice {
                x,
                lo,
                hi,
                max,
                full,
            } => {
                let full = if *full { " full" } else { "" };
                self.line(format_args!("Slice{full}"), span);
                self.nested(|d| {
                    d.expr(*x);
                    d.opt_expr("lo", *lo);
                    d.opt_expr("hi", *hi);
                    d.opt_expr("max", *max);
                });
            }
            ExprKind::Assert { x, ty } => {
                self.line("Assert", span);
                self.nested(|d| {
                    d.expr(*x);
                    d.expr(*ty);
                });
            }
            ExprKind::TypeSwitchGuard { lhs, x } => {
                let lhs = lhs.map_or(String::new(), |lhs| format!(" {}", self.name(lhs)));
                self.line(format_args!("TypeSwitchGuard{lhs}"), span);
                self.nested(|d| d.expr(*x));
            }
            ExprKind::Unary { op, x } => {
                self.line(format_args!("Unary {op:?}"), span);
                self.nested(|d| d.expr(*x));
            }
            ExprKind::Binary { op, x, y } => {
                self.line(format_args!("Binary {op:?}"), span);
                self.nested(|d| {
                    d.expr(*x);
                    d.expr(*y);
                });
            }
            ExprKind::Call {
                fun,
                args,
                spread,
                unwrap,
            } => {
                let unwrap = if *unwrap { " unwrap" } else { "" };
                let spread = if *spread { " spread" } else { "" };
                self.line(format_args!("Call{unwrap}{spread}"), span);
                self.nested(|d| {
                    d.expr(*fun);
                    d.exprs("args", args);
                });
            }
            ExprKind::MacroCall { path, args } => {
                let path = self.macro_ref(path);
                self.line(format_args!("MacroCall {path}"), span);
                self.nested(|d| args.iter().for_each(|&arg| d.expr(arg)));
            }
            ExprKind::ArrayType { len, elem } => {
                self.line("ArrayType", span);
                self.nested(|d| {
                    d.opt_expr("len", *len);
                    d.expr(*elem);
                });
            }
            ExprKind::SliceType { elem } => {
                self.line("SliceType", span);
                self.nested(|d| d.expr(*elem));
            }
            ExprKind::DotsType { elem } => {
                self.line("DotsType", span);
                self.nested(|d| d.expr(*elem));
            }
            ExprKind::StructType { fields } => {
                self.line("StructType", span);
                self.nested(|d| {
                    for field in fields {
                        let names = d.names_of(&field.names);
                        d.line(format_args!("Field {names}"), Some(field.span));
                        d.nested(|d| {
                            d.expr(field.ty);
                            d.attrs(&field.attrs);
                            d.opt_expr("tag", field.tag);
                        });
                    }
                });
            }
            ExprKind::InterfaceType { elems } => {
                self.line("InterfaceType", span);
                self.nested(|d| {
                    for elem in elems {
                        match elem {
                            InterfaceElem::Method { name, ty } => {
                                let span_of_name = name.span;
                                let name = d.name(*name);
                                d.line(format_args!("Method {name}"), Some(span_of_name));
                                d.nested(|d| d.expr(*ty));
                            }
                            InterfaceElem::Type(ty) => d.field("elem", |d| d.expr(*ty)),
                            InterfaceElem::Extends(ty) => d.field("extends", |d| d.expr(*ty)),
                        }
                    }
                });
            }
            ExprKind::FuncType {
                params,
                results,
                fails,
            } => {
                let fails = if *fails { " fails" } else { "" };
                self.line(format_args!("FuncType{fails}"), span);
                self.nested(|d| {
                    d.params("Param", params);
                    d.params("Result", results);
                });
            }
            ExprKind::MapType { key, value } => {
                self.line("MapType", span);
                self.nested(|d| {
                    d.expr(*key);
                    d.expr(*value);
                });
            }
            ExprKind::ChanType { dir, elem } => {
                self.line(format_args!("ChanType {dir:?}"), span);
                self.nested(|d| d.expr(*elem));
            }
        }
    }

    fn stmt(&mut self, id: StmtId) {
        with_stack(|| self.stmt_node(id));
    }

    fn stmt_node(&mut self, id: StmtId) {
        let file = self.file;
        let stmt = &file.stmts[id];
        let span = Some(stmt.span);
        match &stmt.kind {
            StmtKind::Bad => self.line("BadStmt", span),
            StmtKind::Empty => self.line("EmptyStmt", span),
            StmtKind::Labeled { label, stmt } => {
                let label = self.name(*label);
                self.line(format_args!("Labeled {label}"), span);
                self.nested(|d| d.stmt(*stmt));
            }
            StmtKind::Block(block) => self.block(block),
            StmtKind::Expr(expr) => {
                self.line("ExprStmt", span);
                self.nested(|d| d.expr(*expr));
            }
            StmtKind::Send { chan, value } => {
                self.line("Send", span);
                self.nested(|d| {
                    d.expr(*chan);
                    d.expr(*value);
                });
            }
            StmtKind::Decl(decls) => {
                self.line("DeclStmt", span);
                self.nested(|d| decls.iter().for_each(|&decl| d.decl(decl)));
            }
            StmtKind::Assign { op, lhs, rhs } => {
                self.line(format_args!("Assign {op:?}"), span);
                self.nested(|d| {
                    d.exprs("lhs", lhs);
                    d.exprs("rhs", rhs);
                });
            }
            StmtKind::IncDec { x, inc } => {
                let op = if *inc { "++" } else { "--" };
                self.line(format_args!("IncDec {op}"), span);
                self.nested(|d| d.expr(*x));
            }
            StmtKind::Branch { kind, label } => {
                let label = label.map_or(String::new(), |label| format!(" {}", self.name(label)));
                self.line(format_args!("{kind:?}{label}"), span);
            }
            StmtKind::Go(call) | StmtKind::Defer(call) => {
                let kind = if matches!(stmt.kind, StmtKind::Go(_)) {
                    "Go"
                } else {
                    "Defer"
                };
                self.line(kind, span);
                self.nested(|d| d.expr(*call));
            }
            StmtKind::Return(results) => {
                self.line("Return", span);
                self.nested(|d| results.iter().for_each(|&result| d.expr(result)));
            }
            StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            } => {
                self.line("If", span);
                self.nested(|d| {
                    d.opt_stmt("init", *init);
                    d.field("cond", |d| d.expr(*cond));
                    d.block(then);
                    d.opt_stmt("else", *otherwise);
                });
            }
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => {
                self.line("For", span);
                self.nested(|d| {
                    d.opt_stmt("init", *init);
                    d.opt_expr("cond", *cond);
                    d.opt_stmt("post", *post);
                    d.block(body);
                });
            }
            StmtKind::Range {
                lhs,
                define,
                x,
                body,
            } => {
                let define = if *define { " define" } else { "" };
                self.line(format_args!("Range{define}"), span);
                self.nested(|d| {
                    d.exprs("lhs", lhs);
                    d.field("x", |d| d.expr(*x));
                    d.block(body);
                });
            }
            StmtKind::Switch { init, tag, clauses } => {
                self.line("Switch", span);
                self.nested(|d| {
                    d.opt_stmt("init", *init);
                    d.opt_expr("tag", *tag);
                    for clause in clauses {
                        let kind = if clause.cases.is_some() {
                            "Case"
                        } else {
                            "Default"
                        };
                        d.line(kind, Some(clause.span));
                        d.nested(|d| {
                            d.exprs("cases", clause.cases.as_deref().unwrap_or(&[]));
                            d.stmts(&clause.body);
                        });
                    }
                });
            }
            StmtKind::Select { clauses } => {
                self.line("Select", span);
                self.nested(|d| {
                    for clause in clauses {
                        let kind = if clause.comm.is_some() {
                            "Comm"
                        } else {
                            "Default"
                        };
                        d.line(kind, Some(clause.span));
                        d.nested(|d| {
                            d.opt_stmt("comm", clause.comm);
                            d.stmts(&clause.body);
                        });
                    }
                });
            }
        }
    }
}
