//! What the checker found in a package, as text: one line for each entry
//! of each table, in the order of the source. Two checks of one package
//! give the same text, whatever the order in which they worked.

use std::fmt::Write;

use gopp_span::{SourceFile, Span};
use gopp_syntax::ast::{ExprId, File};
use lasso::ThreadedRodeo;

use crate::constant::Value;
use crate::object::{Body, ObjKind};
use crate::package::Package;
use crate::types::Types;

/// The longest piece of source that a line shows.
const SHOWN_AT_MOST: usize = 32;

struct Writer<'w> {
    file: &'w File,
    source: &'w SourceFile,
    types: &'w Types,
    names: &'w ThreadedRodeo,
    lines: Vec<(Span, &'static str, String)>,
}

impl Writer<'_> {
    /// `line:col`, and the source of the expression on one line.
    fn place(&self, span: Span) -> String {
        let at = self.source.line_col(span.lo);
        let text = &self.source.text()[span.lo as usize..span.hi as usize];
        let mut shown: String = text.split_whitespace().collect::<Vec<_>>().join(" ");
        if shown.chars().count() > SHOWN_AT_MOST {
            shown = shown.chars().take(SHOWN_AT_MOST - 1).collect::<String>() + "…";
        }
        format!("{}:{} {shown}", at.line, at.column)
    }

    fn add(&mut self, table: &'static str, expr: ExprId, what: String) {
        self.lines.push((self.file.exprs[expr].span, table, what));
    }

    fn body(&mut self, body: &Body) {
        let show = |ty| self.types.display(ty, self.names);
        let name = |spur| self.names.resolve(&spur);
        let mut found: Vec<(&'static str, ExprId, String)> = Vec::new();
        for (&expr, typed) in &body.types {
            let value = match &typed.value {
                Value::Unknown => String::new(),
                value => format!(" = {value}"),
            };
            found.push(("type", expr, format!("{}{value}", show(typed.ty))));
        }
        for (&expr, selection) in &body.selections {
            let what = format!(
                "{:?} of {} in {} by {:?}{}",
                selection.kind,
                show(selection.recv),
                show(selection.holder),
                selection.path,
                if selection.indirect {
                    " through a pointer"
                } else {
                    ""
                }
            );
            found.push(("selection", expr, what));
        }
        for (&expr, instance) in &body.instances {
            let args: Vec<String> = instance.args.iter().map(|&arg| show(arg)).collect();
            found.push((
                "instance",
                expr,
                format!("[{}] {}", args.join(", "), show(instance.ty)),
            ));
        }
        for (&expr, &(from, to)) in &body.iface_conversions {
            found.push(("boxed", expr, format!("{} to {}", show(from), show(to))));
        }
        for (&expr, bang) in &body.bangs {
            found.push(("bang", expr, format!("{bang:?}")));
        }
        for (&expr, mode) in &body.var_modes {
            found.push(("given", expr, format!("{mode:?}")));
        }
        for (&expr, value) in &body.method_values {
            found.push(("method value", expr, format!("{value:?}")));
        }
        for (&expr, captures) in &body.captures {
            let all: Vec<String> = captures
                .iter()
                .map(|capture| {
                    let local = &body.locals[capture.local.0 as usize];
                    format!("{} {:?}", name(local.name), capture.how)
                })
                .collect();
            found.push(("captures", expr, all.join(", ")));
        }
        let tables = [
            ("operator", &body.operators),
            ("op assign", &body.op_assigns),
            ("index store", &body.index_stores),
            ("case operator", &body.case_operators),
        ];
        for (table, uses) in tables {
            for (&expr, used) in uses {
                let right = if used.swapped { " right" } else { "" };
                let what = format!(
                    "{}.{} {}{right}",
                    show(used.holder),
                    name(used.name),
                    show(used.sig)
                );
                found.push((table, expr, what));
            }
        }
        for (&expr, used) in &body.foreign_uses {
            found.push((
                "foreign",
                expr,
                format!("{} {}", name(used.name), show(used.ty)),
            ));
        }
        let marked = [
            ("swap", &body.swaps),
            ("self move", &body.self_moves),
            ("narrowed", &body.narrowed),
            ("interface argument", &body.iface_args),
            ("writes through", &body.writes_through),
        ];
        for (table, exprs) in marked {
            for &expr in exprs {
                found.push((table, expr, String::new()));
            }
        }
        for (table, expr, what) in found {
            self.add(table, expr, what);
        }
    }
}

/// The tables of the package `package`, whose files are `files` in the
/// order in which they were checked.
pub fn info_text(
    package: &Package,
    files: &[(&File, &SourceFile)],
    types: &Types,
    names: &ThreadedRodeo,
) -> String {
    let mut out = format!("package {} ({})\n", package.name, package.path);
    for object in &package.objects {
        let kind = match object.kind {
            ObjKind::Const => "const",
            ObjKind::Type => "type",
            ObjKind::Var => "var",
            ObjKind::Func => "func",
            ObjKind::Macro | ObjKind::Import { .. } => continue,
        };
        let value = match &object.value {
            Value::Unknown => String::new(),
            value => format!(" = {value}"),
        };
        let ty = types.display(object.ty, names);
        let _ = writeln!(out, "{kind} {} {ty}{value}", names.resolve(&object.name));
    }
    for ((file, source), info) in files.iter().zip(&package.files) {
        let _ = writeln!(out, "file {}", source.path().display());
        let mut writer = Writer {
            file,
            source,
            types,
            names,
            lines: Vec::new(),
        };
        for body in &info.bodies {
            writer.body(body);
        }
        let mut lines = std::mem::take(&mut writer.lines);
        lines.sort_by(|a, b| (a.0.lo, a.0.hi, a.1, &a.2).cmp(&(b.0.lo, b.0.hi, b.1, &b.2)));
        for (span, table, what) in lines {
            let _ = writeln!(out, "  {}: {table} {what}", writer.place(span));
        }
    }
    out
}
