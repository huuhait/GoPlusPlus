//! Binds every identifier to what it denotes: Go's scope rules, with the
//! messages of Go's type checker.
//!
//! The package-level names of all files are collected first. Each top-level
//! declaration is then resolved on its own, in parallel; what the threads
//! find is joined in source order.

use std::sync::Arc;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{Dialect, SourceFile, Span};
use gopp_syntax::ast::*;
use gopp_syntax::constraint::is_build;
use gopp_syntax::with_stack;
use lasso::{Spur, ThreadedRodeo};
use rayon::prelude::*;
use rustc_hash::{FxHashMap, FxHashSet};

use crate::constant::Value;
use crate::decls;
use crate::literal::unquote;
use crate::object::{Body, FileInfo, Local, LocalId, LocalKind, ObjId, ObjKind, Object, Ref};
use crate::package::{Package, PackageLoader};
use crate::typecheck;
use crate::types::Types;
use crate::universe::{PredeclaredKind, Universe};

/// One file of the package to check.
#[derive(Clone, Copy)]
pub struct Unit<'a> {
    pub file: &'a File,
    pub source: &'a SourceFile,
}

/// Checks the package at the import path `path`, made of `units`.
///
/// `names` and `types` must be those of every package that `loader`
/// returns.
pub fn check(
    path: &str,
    units: &[Unit<'_>],
    loader: &dyn PackageLoader,
    names: &ThreadedRodeo,
    types: &Types,
    sink: &Sink,
) -> Package {
    check_counted(path, units, loader, names, types, sink).0
}

/// The package and the number of steps the checker took.
pub(crate) fn check_counted(
    path: &str,
    units: &[Unit<'_>],
    loader: &dyn PackageLoader,
    names: &ThreadedRodeo,
    types: &Types,
    sink: &Sink,
) -> (Package, u64) {
    let package_name = units.iter().find_map(|unit| unit.file.package);
    let mut collector = Collector {
        names,
        sink,
        loader,
        blank: names.get_or_intern_static("_"),
        package: Package {
            path: path.to_owned(),
            name: package_name.map_or(String::new(), |name| names.resolve(&name.name).to_owned()),
            ..Package::default()
        },
        file_scopes: Vec::new(),
    };
    for (index, unit) in units.iter().enumerate() {
        collector.file(index, unit);
    }
    collector.check_file_scopes();
    let blank = collector.blank;
    let mut package = collector.package;

    let jobs: Vec<(usize, usize)> = units
        .iter()
        .enumerate()
        .flat_map(|(file, unit)| (0..unit.file.decls.len()).map(move |decl| (file, decl)))
        .collect();
    let resolved: Vec<Resolved> = {
        let cx = Context {
            names,
            package: &package,
            universe: Universe::new(names),
            blank,
        };
        let resolve = |&(file, decl): &(usize, usize)| cx.resolve(file, units[file], decl);
        jobs.par_iter().map(resolve).collect()
    };

    let mut steps = 0;
    for (&(file, _), resolved) in jobs.iter().zip(resolved) {
        for diagnostic in resolved.diagnostics {
            sink.push(diagnostic);
        }
        for import in resolved.used_imports {
            if let ObjKind::Import { used, .. } = &mut package.objects[import.0 as usize].kind {
                *used = true;
            }
        }
        steps += resolved.steps;
        package.files[file].bodies.push(resolved.body);
    }
    for object in &package.objects {
        if let Some(diagnostic) = unused_import(object, names) {
            sink.push(diagnostic);
        }
    }
    let (needs, methods, taken) = typecheck::declarations(&mut package, units, types, names, sink);
    steps += taken;
    package.init_order = decls::order(&package, units, &needs, &methods, names, sink);
    // After the types: a variable that got no type is not also unused.
    for local in package
        .files
        .iter()
        .flat_map(|file| &file.bodies)
        .flat_map(|body| &body.locals)
    {
        if local.kind == LocalKind::Var && !local.used {
            let message = format!("declared and not used: {}", names.resolve(&local.name));
            sink.push(Diagnostic::new(Code::DeclaredNotUsed, local.span, message));
        }
    }
    (package, steps)
}

fn unused_import(object: &Object, names: &ThreadedRodeo) -> Option<Diagnostic> {
    let ObjKind::Import {
        package,
        used: false,
        ..
    } = &object.kind
    else {
        return None;
    };
    let path = &package.path;
    let name = names.resolve(&object.name);
    // The name is shown when it is not what the path ends in.
    let last = path.rsplit('/').next().unwrap_or(path);
    Some(if name == "." || name == last {
        let message = format!("{path:?} imported and not used");
        Diagnostic::new(Code::UnusedImport, object.span, message)
    } else {
        let message = format!("{path:?} imported as {name} and not used");
        Diagnostic::new(Code::UnusedImportAs, object.span, message)
    })
}

fn is_exported(name: &str) -> bool {
    name.starts_with(char::is_uppercase)
}

fn redeclared(name: &str, span: Span, other: Option<Span>) -> Diagnostic {
    let message = format!("{name} redeclared in this block");
    Diagnostic::new(Code::Redeclared, span, message).with_note(
        Code::NoteOtherDeclaration,
        other,
        format!("other declaration of {name}"),
    )
}

/// Declares the package-level names and the imports of every file.
struct Collector<'a> {
    names: &'a ThreadedRodeo,
    sink: &'a Sink,
    loader: &'a dyn PackageLoader,
    blank: Spur,
    package: Package,
    /// The names of each file scope in the order they were declared.
    file_scopes: Vec<Vec<Spur>>,
}

impl Collector<'_> {
    fn file(&mut self, index: usize, unit: &Unit<'_>) {
        let file = unit.file;
        self.package.files.push(FileInfo::default());
        self.file_scopes.push(Vec::new());
        for &decl in &file.decls {
            match &file.decl_nodes[decl].kind {
                DeclKind::Import { name, path } => self.import(index, unit, *name, *path),
                DeclKind::Const { names, .. } => {
                    names
                        .iter()
                        .for_each(|&name| self.declare(name, ObjKind::Const));
                }
                DeclKind::Var { names, .. } => {
                    names
                        .iter()
                        .for_each(|&name| self.declare(name, ObjKind::Var));
                }
                DeclKind::Type { name, .. } => self.declare(*name, ObjKind::Type),
                DeclKind::Macro { name, .. }
                | DeclKind::Func {
                    is_macro: true,
                    name,
                    ..
                } => self.declare(*name, ObjKind::Macro),
                // A method is found through its type, and a package may
                // have any number of `init` functions.
                DeclKind::Func {
                    receiver: None,
                    name,
                    ..
                } if self.names.resolve(&name.name) != "init" => {
                    self.declare(*name, ObjKind::Func);
                }
                DeclKind::Func { .. } | DeclKind::MacroCall(_) | DeclKind::Bad => {}
            }
        }
    }

    fn object(&mut self, name: Spur, kind: ObjKind, span: Span) -> ObjId {
        let id = ObjId(self.package.objects.len() as u32);
        self.package.objects.push(Object {
            name,
            kind,
            span,
            ty: Types::INVALID,
            value: Value::Unknown,
        });
        id
    }

    /// Declares `name` in the package scope, or among the macros.
    fn declare(&mut self, name: Ident, kind: ObjKind) {
        if name.name == self.blank {
            // `var _ = f()` names nothing, but its value is computed when
            // the program starts; a constant or a type without a name is
            // checked like any other.
            if matches!(kind, ObjKind::Var | ObjKind::Const | ObjKind::Type) {
                self.object(name.name, kind, name.span);
            }
            return;
        }
        let is_macro = matches!(kind, ObjKind::Macro);
        // `init`, and `main` in package main, are functions and nothing
        // else: what else has the name is not declared.
        let text = self.names.resolve(&name.name);
        let entry_point = text == "init" || text == "main" && self.package.name == "main";
        if entry_point && matches!(kind, ObjKind::Const | ObjKind::Var | ObjKind::Type) {
            let message = format!("cannot declare {text} - must be func");
            let diagnostic = Diagnostic::new(Code::InitSignature, name.span, message);
            self.sink.push(diagnostic);
            return;
        }
        let scope = if is_macro {
            &self.package.macros
        } else {
            &self.package.scope
        };
        if let Some(&other) = scope.get(&name.name) {
            let other = self.package.object(other).span;
            let text = self.names.resolve(&name.name);
            self.sink.push(redeclared(text, name.span, Some(other)));
            return;
        }
        let id = self.object(name.name, kind, name.span);
        let scope = if is_macro {
            &mut self.package.macros
        } else {
            &mut self.package.scope
        };
        scope.insert(name.name, id);
    }

    fn import(&mut self, index: usize, unit: &Unit<'_>, name: Option<Ident>, path: ExprId) {
        let literal = &unit.file.exprs[path];
        let is_string = matches!(
            literal.kind,
            ExprKind::BasicLit {
                kind: LitKind::String,
                bad: false
            }
        );
        if !is_string {
            return;
        }
        let text = &unit.source.text()[literal.span.lo as usize..literal.span.hi as usize];
        let import_path = unquote(text).and_then(|bytes| String::from_utf8(bytes).ok());
        let Some(import_path) = import_path.filter(|path| !path.is_empty()) else {
            let message = "invalid import path (empty string)";
            let diagnostic = Diagnostic::new(Code::BadImportPath, literal.span, message);
            self.sink.push(diagnostic);
            return;
        };
        // Go's rule for a path; a `.gopp` file also imports headers, as
        // `"<vector>"`.
        const ILLEGAL: &str = "!\"#$%&'()*,:;<=>?[\\]^{|}`\u{FFFD}";
        let illegal = |c: char| c.is_control() || c.is_whitespace() || ILLEGAL.contains(c);
        let go = unit.source.dialect() == Dialect::Go;
        if let Some(c) = import_path.chars().find(|&c| go && illegal(c)) {
            let shown = match c.is_control() {
                true => format!("U+{:04X}", c as u32),
                false => format!("U+{:04X} '{c}'", c as u32),
            };
            let message = format!("invalid import path (invalid character {shown})");
            let diagnostic = Diagnostic::new(Code::BadImportPath, literal.span, message);
            self.sink.push(diagnostic);
            return;
        }
        if let Some(name) = name.filter(|name| self.names.resolve(&name.name) == "init") {
            let message = "cannot import package as init - init must be a func";
            let diagnostic = Diagnostic::new(Code::InitSignature, name.span, message);
            self.sink.push(diagnostic);
            return;
        }
        let imported = self.loader.load(&import_path).unwrap_or_else(|reason| {
            let message = format!("could not import {import_path} ({reason})");
            let diagnostic = Diagnostic::new(Code::ImportUnresolved, literal.span, message);
            self.sink.push(diagnostic);
            Arc::new(Package::fake(&import_path))
        });

        let (spur, span) = match name {
            Some(name) => (name.name, name.span),
            None => (self.names.get_or_intern(&imported.name), literal.span),
        };
        if spur == self.blank {
            return;
        }
        let kind = ObjKind::Import {
            package: Arc::clone(&imported),
            file: index as u32,
            // A package that could not be loaded is not reported twice.
            used: imported.fake,
        };
        let import = self.object(spur, kind, span);
        if self.names.resolve(&spur) != "." {
            self.declare_in_file(index, spur, Ref::Object(import), span);
            return;
        }
        // `import . "p"` puts the exported names of `p` into the file scope.
        for (member, object) in imported.objects.iter().enumerate() {
            let member = ObjId(member as u32);
            let exported = imported.scope.get(&object.name) == Some(&member)
                && is_exported(self.names.resolve(&object.name));
            if exported {
                let denotes = Ref::Member { import, member };
                self.declare_in_file(index, object.name, denotes, span);
            }
        }
    }

    /// Where the import behind a name of a file scope was declared. A file
    /// scope holds imports and their members only.
    fn import_span(&self, denotes: Ref) -> Option<Span> {
        match denotes {
            Ref::Object(import) | Ref::Member { import, .. } => {
                Some(self.package.object(import).span)
            }
            Ref::Universe(_) | Ref::Local(_) => None,
        }
    }

    fn declare_in_file(&mut self, index: usize, name: Spur, denotes: Ref, span: Span) {
        if let Some(&other) = self.package.files[index].scope.get(&name) {
            let text = self.names.resolve(&name);
            self.sink
                .push(redeclared(text, span, self.import_span(other)));
            return;
        }
        self.package.files[index].scope.insert(name, denotes);
        self.file_scopes[index].push(name);
    }

    /// A name of a file scope may not be a name of the package scope.
    fn check_file_scopes(&self) {
        for (file, names) in self.package.files.iter().zip(&self.file_scopes) {
            for name in names {
                let Some(&declared) = self.package.scope.get(name) else {
                    continue;
                };
                let denotes = file.scope[name];
                let (Ref::Object(import) | Ref::Member { import, .. }) = denotes else {
                    continue;
                };
                let ObjKind::Import { package, .. } = &self.package.object(import).kind else {
                    continue;
                };
                let how = match denotes {
                    Ref::Member { .. } => "dot-import",
                    _ => "import",
                };
                let text = self.names.resolve(name);
                let message = format!(
                    "{text} already declared through {how} of package {} ({:?})",
                    package.name, package.path
                );
                let span = self.package.object(declared).span;
                let diagnostic = Diagnostic::new(Code::Redeclared, span, message).with_note(
                    Code::NoteOtherDeclaration,
                    self.import_span(denotes),
                    format!("other declaration of {text}"),
                );
                self.sink.push(diagnostic);
            }
        }
    }
}

/// What the threads that resolve declarations share.
struct Context<'a> {
    names: &'a ThreadedRodeo,
    package: &'a Package,
    universe: Universe,
    blank: Spur,
}

/// What one thread found in one top-level declaration.
struct Resolved {
    body: Body,
    used_imports: FxHashSet<ObjId>,
    diagnostics: Vec<Diagnostic>,
    steps: u64,
}

/// The local that a name is bound to, and the scope that binds it.
#[derive(Clone, Copy)]
struct Binding {
    local: LocalId,
    scope: u32,
}

impl Context<'_> {
    fn resolve(&self, file: usize, unit: Unit<'_>, decl: usize) -> Resolved {
        let mut resolver = Resolver {
            cx: self,
            file: unit.file,
            text: unit.source.text(),
            dialect: unit.source.dialect(),
            file_scope: &self.package.files[file].scope,
            body: Body::default(),
            visible: FxHashMap::default(),
            undo: Vec::new(),
            scopes: Vec::new(),
            next_scope: 0,
            seen: FxHashSet::default(),
            results: Vec::new(),
            loop_vars: FxHashMap::default(),
            lenient: false,
            in_method: false,
            used_imports: FxHashSet::default(),
            diagnostics: Vec::new(),
            steps: 0,
        };
        resolver.top_level(unit.file.decls[decl]);
        Resolved {
            body: resolver.body,
            used_imports: resolver.used_imports,
            diagnostics: resolver.diagnostics,
            steps: resolver.steps,
        }
    }
}

struct Resolver<'a> {
    cx: &'a Context<'a>,
    file: &'a File,
    text: &'a str,
    dialect: Dialect,
    file_scope: &'a FxHashMap<Spur, Ref>,
    body: Body,
    /// The innermost local of each name that is in scope.
    visible: FxHashMap<Spur, Binding>,
    /// What each declaration hid, to bring back when its scope ends.
    undo: Vec<(Spur, Option<Binding>)>,
    /// The open scopes: their number, and where their part of `undo` starts.
    scopes: Vec<(u32, usize)>,
    next_scope: u32,
    /// The names on the left of the `:=` being resolved.
    seen: FxHashSet<Spur>,
    /// The results with a name of each function that is being resolved,
    /// the innermost last.
    results: Vec<Vec<(Spur, LocalId)>>,
    /// In a `.gopp` file: the variables of the loops whose bodies are
    /// being resolved, each with where it is declared.
    loop_vars: FxHashMap<Spur, Vec<Span>>,
    /// Inside the arguments of a macro or an attribute, where a name may be
    /// a word of the macro's own: an unknown name is no error there.
    lenient: bool,
    /// Inside a method, where `base` names the parents of its type.
    in_method: bool,
    used_imports: FxHashSet<ObjId>,
    diagnostics: Vec<Diagnostic>,
    steps: u64,
}

impl<'a> Resolver<'a> {
    fn error(&mut self, code: Code, span: Span, message: impl Into<String>) {
        self.diagnostics.push(Diagnostic::new(code, span, message));
    }

    fn name_text(&self, name: Spur) -> &'a str {
        self.cx.names.resolve(&name)
    }

    // ---- scopes

    fn open(&mut self) {
        self.scopes.push((self.next_scope, self.undo.len()));
        self.next_scope += 1;
    }

    fn close(&mut self) {
        let Some((_, start)) = self.scopes.pop() else {
            return;
        };
        for (name, hidden) in self.undo.drain(start..).rev() {
            match hidden {
                Some(binding) => self.visible.insert(name, binding),
                None => self.visible.remove(&name),
            };
        }
    }

    fn scope(&self) -> u32 {
        self.scopes.last().map_or(0, |&(scope, _)| scope)
    }

    fn local(&mut self, ident: Ident, kind: LocalKind) -> LocalId {
        let id = LocalId(self.body.locals.len() as u32);
        self.body.locals.push(Local {
            name: ident.name,
            kind,
            span: ident.span,
            used: false,
            ty: Types::INVALID,
        });
        id
    }

    fn bind(&mut self, name: Spur, local: LocalId) {
        let binding = Binding {
            local,
            scope: self.scope(),
        };
        let hidden = self.visible.insert(name, binding);
        self.undo.push((name, hidden));
    }

    /// Declares `ident` in the innermost scope and returns the new local.
    /// `_` declares nothing.
    fn declare(&mut self, ident: Ident, kind: LocalKind) -> Option<LocalId> {
        self.steps += 1;
        if ident.name == self.cx.blank {
            return None;
        }
        if let Some(other) = self.visible.get(&ident.name)
            && other.scope == self.scope()
        {
            let other = self.body.locals[other.local.0 as usize].span;
            let text = self.name_text(ident.name);
            self.diagnostics
                .push(redeclared(text, ident.span, Some(other)));
            return None;
        }
        // A variable of a loop keeps its name to itself in the body of
        // the loop.
        let is_variable = !matches!(kind, LocalKind::Const | LocalKind::Type);
        let loop_var = self.loop_vars.get(&ident.name).and_then(|at| at.last());
        if let (true, Some(&declared)) = (is_variable, loop_var) {
            let text = self.name_text(ident.name);
            let message = format!("{text} redeclares a loop variable: use another name");
            let diagnostic = Diagnostic::new(Code::LoopVarRedeclared, ident.span, message)
                .with_note(
                    Code::NoteLoopVarDeclaredHere,
                    Some(declared),
                    format!("loop variable {text} declared here"),
                );
            self.diagnostics.push(diagnostic);
        }
        let local = self.local(ident, kind);
        self.body.defs.insert(ident.span.lo, local);
        self.bind(ident.name, local);
        Some(local)
    }

    /// The body of a loop whose variables are among the locals
    /// `declared`: those that are in scope now.
    fn loop_body(&mut self, declared: std::ops::Range<usize>, body: &Block) {
        let mut own: Vec<(Spur, Span)> = Vec::new();
        for index in declared.filter(|_| self.dialect == Dialect::Gopp) {
            let local = &self.body.locals[index];
            let in_scope = self.visible.get(&local.name).map(|binding| binding.local);
            if in_scope == Some(LocalId(index as u32)) {
                own.push((local.name, local.span));
            }
        }
        for &(name, span) in &own {
            self.loop_vars.entry(name).or_default().push(span);
        }
        self.block(body);
        for (name, _) in own {
            if let Some(declared) = self.loop_vars.get_mut(&name) {
                declared.pop();
            }
        }
    }

    /// Declares a constant or a variable. If its value is in error, the
    /// variable is not also reported as unused.
    fn declare_valued(&mut self, ident: Ident, kind: LocalKind, in_error: bool) {
        if let Some(local) = self.declare(ident, kind) {
            self.body.locals[local.0 as usize].used = in_error;
        }
    }

    fn use_import(&mut self, denotes: Ref) {
        if let Ref::Object(import) | Ref::Member { import, .. } = denotes {
            self.used_imports.insert(import);
        }
    }

    /// What `name` denotes here: a local, then a name of the file scope, of
    /// the package scope, of the universe.
    fn lookup(&mut self, name: Spur, is_use: bool) -> Option<Ref> {
        self.steps += 1;
        if let Some(binding) = self.visible.get(&name) {
            self.body.locals[binding.local.0 as usize].used |= is_use;
            return Some(Ref::Local(binding.local));
        }
        if let Some(&denotes) = self.file_scope.get(&name) {
            self.use_import(denotes);
            return Some(denotes);
        }
        if let Some(&object) = self.cx.package.scope.get(&name) {
            return Some(Ref::Object(object));
        }
        let predeclared = self.cx.universe.lookup(self.dialect, name);
        predeclared.map(Ref::Universe)
    }

    /// Resolves the name `id`. `is_use` says whether this reads the value
    /// of a variable.
    fn name(&mut self, id: ExprId, name: Spur, is_use: bool) {
        let span = self.file.exprs[id].span;
        if name == self.cx.blank {
            if !self.lenient {
                let message = "cannot use _ as value or type";
                self.error(Code::CannotUseBlank, span, message);
            }
            return;
        }
        if let Some(denotes) = self.lookup(name, is_use) {
            self.body.uses.insert(id, denotes);
            return;
        }
        if self.lenient {
            return;
        }
        let text = self.name_text(name);
        // In a method of a `.gopp` file `base` names the parents of the
        // type, if it has any, which its declaration says.
        if text == "base" && self.in_method && self.dialect == Dialect::Gopp {
            return;
        }
        let mut diagnostic = Diagnostic::new(Code::Undefined, span, format!("undefined: {text}"));
        if text == "new" && self.dialect == Dialect::Gopp {
            let note = "Go++ has no new: write NewUnique(T{}) for one owner or NewShared(T{}) \
                        for several";
            diagnostic = diagnostic.with_note(Code::NoteOwnNewRemoved, None, note);
        }
        self.diagnostics.push(diagnostic);
    }

    /// The import that `name` denotes, if no local hides it.
    /// A name as the length of an array: `[N]T` with a name that is no
    /// constant is likely a type parameter that lost its constraint.
    fn array_length(&mut self, id: ExprId, name: Spur) {
        if self.lenient {
            return self.name(id, name, true);
        }
        let span = self.file.exprs[id].span;
        let text = self.name_text(name);
        let Some(denotes) = self.lookup(name, true) else {
            let message = format!("undefined array length {text} or missing type constraint");
            return self.error(Code::InvalidArrayLength, span, message);
        };
        self.body.uses.insert(id, denotes);
        let constant = match denotes {
            Ref::Universe(name) => name.kind() == PredeclaredKind::Const,
            Ref::Object(object) => matches!(self.cx.package.object(object).kind, ObjKind::Const),
            Ref::Member { import, member } => match &self.cx.package.object(import).kind {
                ObjKind::Import { package, .. } => {
                    matches!(package.object(member).kind, ObjKind::Const)
                }
                _ => false,
            },
            Ref::Local(local) => self.body.locals[local.0 as usize].kind == LocalKind::Const,
        };
        if !constant {
            let message = format!("invalid array length {text}");
            self.error(Code::InvalidArrayLength, span, message);
        }
    }

    fn import_named(&mut self, name: Spur) -> Option<ObjId> {
        self.steps += 1;
        if self.visible.contains_key(&name) {
            return None;
        }
        match self.file_scope.get(&name) {
            Some(&Ref::Object(import)) => {
                self.used_imports.insert(import);
                Some(import)
            }
            _ => None,
        }
    }

    // ---- declarations

    fn top_level(&mut self, id: DeclId) {
        let file = self.file;
        let decl = &file.decl_nodes[id];
        // The attributes of a type are resolved with its type
        // parameters: `@Extends(A[T])` names them.
        if !matches!(decl.kind, DeclKind::Type { .. }) {
            self.attrs(&decl.attrs);
        }
        match &decl.kind {
            DeclKind::Const { ty, values, .. } | DeclKind::Var { ty, values, .. } => {
                self.opt_expr(*ty);
                self.exprs(values);
            }
            DeclKind::Type {
                type_params, ty, ..
            } => self.type_decl(&decl.attrs, type_params, *ty),
            DeclKind::Func {
                receiver,
                type_params,
                ty,
                body,
                ..
            } => self.function(receiver.as_ref(), type_params, *ty, body.as_ref()),
            DeclKind::MacroCall(call) => self.expr(*call),
            // The body of a declarative macro is made of fragments: it is
            // resolved where the macro is expanded.
            DeclKind::Macro { .. } | DeclKind::Import { .. } | DeclKind::Bad => {}
        }
    }

    /// The type parameters and the type of a type declaration.
    fn type_decl(&mut self, attrs: &[Attribute], type_params: &[TypeParam], ty: ExprId) {
        self.open();
        self.type_params(type_params);
        self.attrs(attrs);
        self.expr(ty);
        self.close();
    }

    /// Declares the type parameters, then resolves their constraints, so
    /// that a constraint may name a later parameter.
    fn type_params(&mut self, type_params: &[TypeParam]) {
        for param in type_params {
            self.declare(param.name, LocalKind::TypeParam);
        }
        let mut last = None;
        for param in type_params {
            // `[A, B any]` gives both the same constraint.
            if last.replace(param.bound) != Some(param.bound) {
                self.expr(param.bound);
            }
        }
    }

    /// A function with its own scope: a declaration, a literal, or only a
    /// signature. The scope of a parameter starts after the signature, so
    /// the type of one parameter never names another.
    fn function(
        &mut self,
        receiver: Option<&Param>,
        type_params: &[TypeParam],
        ty: ExprId,
        body: Option<&Block>,
    ) {
        let file = self.file;
        let around = self.in_method;
        self.in_method |= receiver.is_some();
        self.open();
        if let Some(receiver) = receiver {
            self.receiver_type(receiver.ty);
        }
        self.type_params(type_params);
        let (params, results): (&[Param], &[Param]) = match &file.exprs[ty].kind {
            ExprKind::FuncType {
                params, results, ..
            } => (params, results),
            _ => (&[], &[]),
        };
        let mut last = None;
        for param in params.iter().chain(results) {
            // `a, b int` gives both the same type.
            if last.replace(param.ty) != Some(param.ty) {
                self.expr(param.ty);
            }
        }
        let kinds = [
            (
                receiver.map_or(&[][..], std::slice::from_ref),
                LocalKind::Receiver,
            ),
            (params, LocalKind::Param),
            (results, LocalKind::Result),
        ];
        let mut named = Vec::new();
        for (list, kind) in kinds {
            for name in list.iter().filter_map(|param| param.name) {
                let local = self.declare(name, kind);
                if let (LocalKind::Result, Some(local)) = (kind, local) {
                    named.push((name.name, local));
                }
            }
        }
        self.results.push(named);
        if let Some(body) = body {
            self.stmts(&body.stmts);
        }
        self.results.pop();
        self.close();
        self.in_method = around;
    }

    /// The type of a receiver: in `T[P, Q]` the names in brackets declare
    /// the type parameters of the method.
    fn receiver_type(&mut self, ty: ExprId) {
        let file = self.file;
        let mut base = ty;
        while let ExprKind::Paren(x)
        | ExprKind::Unary {
            op: UnaryOp::Star,
            x,
        } = file.exprs[base].kind
        {
            base = x;
        }
        let ExprKind::Index { x, indices } = &file.exprs[base].kind else {
            self.expr(base);
            return;
        };
        self.expr(*x);
        for &index in indices {
            let node = &file.exprs[index];
            match node.kind {
                ExprKind::Name(name) => {
                    let ident = Ident {
                        name,
                        span: node.span,
                    };
                    // The name also stands for its parameter in the type
                    // of the receiver.
                    if let Some(local) = self.declare(ident, LocalKind::TypeParam) {
                        self.body.uses.insert(index, Ref::Local(local));
                    }
                }
                _ => self.expr(index),
            }
        }
    }

    /// A `const`, `var` or `type` statement inside a function.
    fn local_decl(&mut self, id: DeclId) {
        let file = self.file;
        match &file.decl_nodes[id].kind {
            // The scope of a constant or variable starts after its
            // declaration: `x := x` reads the outer `x`.
            DeclKind::Const { names, ty, values } | DeclKind::Var { names, ty, values } => {
                self.opt_expr(*ty);
                let errors = self.diagnostics.len();
                self.exprs(values);
                let in_error = self.diagnostics.len() > errors;
                let is_const = matches!(file.decl_nodes[id].kind, DeclKind::Const { .. });
                let kind = if is_const {
                    LocalKind::Const
                } else {
                    LocalKind::Var
                };
                for &name in names {
                    self.declare_valued(name, kind, in_error);
                }
            }
            // The scope of a type starts at its name, so it may name itself.
            DeclKind::Type {
                name,
                type_params,
                ty,
                ..
            } => {
                self.declare(*name, LocalKind::Type);
                self.type_decl(&[], type_params, *ty);
            }
            _ => {}
        }
    }

    fn attrs(&mut self, attrs: &[Attribute]) {
        for attr in attrs {
            // The words of a build condition are not names of the program.
            if is_build(attr, self.text) {
                continue;
            }
            if let Some(package) = attr.path.package {
                self.import_named(package.name);
            }
            self.macro_args(attr.args.as_deref().unwrap_or(&[]));
        }
    }

    fn macro_args(&mut self, args: &[ExprId]) {
        let lenient = std::mem::replace(&mut self.lenient, true);
        self.exprs(args);
        self.lenient = lenient;
    }

    // ---- expressions

    fn exprs(&mut self, exprs: &[ExprId]) {
        for &expr in exprs {
            self.expr(expr);
        }
    }

    fn opt_expr(&mut self, expr: Option<ExprId>) {
        if let Some(expr) = expr {
            self.expr(expr);
        }
    }

    fn expr(&mut self, id: ExprId) {
        with_stack(|| self.expr_node(id));
    }

    fn expr_node(&mut self, id: ExprId) {
        let file = self.file;
        self.steps += 1;
        match &file.exprs[id].kind {
            ExprKind::Bad | ExprKind::BasicLit { .. } => {}
            ExprKind::Name(name) => self.name(id, *name, true),
            ExprKind::CompositeLit { ty, elems } => {
                self.opt_expr(*ty);
                for &elem in elems {
                    match file.exprs[elem].kind {
                        ExprKind::KeyValue { key, value } => {
                            self.key(key);
                            self.expr(value);
                        }
                        _ => self.expr(elem),
                    }
                }
            }
            ExprKind::FuncLit { ty, body } => self.function(None, &[], *ty, Some(body)),
            ExprKind::FuncType { .. } => self.function(None, &[], id, None),
            ExprKind::Paren(x)
            | ExprKind::Unary { x, .. }
            | ExprKind::TypeSwitchGuard { x, .. }
            | ExprKind::SliceType { elem: x }
            | ExprKind::DotsType { elem: x }
            | ExprKind::ChanType { elem: x, .. } => self.expr(*x),
            ExprKind::Selector { x, sel } => self.selector(id, *x, *sel),
            ExprKind::Index { x, indices } => {
                self.expr(*x);
                self.exprs(indices);
            }
            ExprKind::Slice { x, lo, hi, max, .. } => {
                self.expr(*x);
                self.opt_expr(*lo);
                self.opt_expr(*hi);
                self.opt_expr(*max);
            }
            ExprKind::KeyValue { key: x, value: y }
            | ExprKind::Assert { x, ty: y }
            | ExprKind::Binary { x, y, .. }
            | ExprKind::MapType { key: x, value: y } => {
                self.expr(*x);
                self.expr(*y);
            }
            ExprKind::Call { fun, args, .. } => {
                self.expr(*fun);
                self.exprs(args);
            }
            ExprKind::MacroCall { path, args } => {
                match path.package {
                    Some(package) => {
                        self.import_named(package.name);
                    }
                    None => {
                        if let Some(&found) = self.cx.package.macros.get(&path.name.name) {
                            self.body.uses.insert(id, Ref::Object(found));
                        }
                    }
                }
                self.macro_args(args);
            }
            ExprKind::ArrayType { len, elem } => {
                match len.map(|len| (len, &file.exprs[len].kind)) {
                    Some((len, &ExprKind::Name(name))) if name != self.cx.blank => {
                        self.array_length(len, name);
                    }
                    _ => self.opt_expr(*len),
                }
                self.expr(*elem);
            }
            ExprKind::StructType { fields } => {
                for field in fields {
                    self.expr(field.ty);
                    self.attrs(&field.attrs);
                }
            }
            ExprKind::InterfaceType { elems } => {
                for elem in elems {
                    let (InterfaceElem::Method { ty, .. } | InterfaceElem::Type(ty)) = elem;
                    self.expr(*ty);
                }
            }
        }
    }

    /// The key of an element of a composite literal. A name may be a field
    /// of the struct, which only the type of the literal tells; what it
    /// denotes otherwise is kept apart, and no name is undefined here.
    fn key(&mut self, key: ExprId) {
        let ExprKind::Name(name) = self.file.exprs[key].kind else {
            self.expr(key);
            return;
        };
        if name != self.cx.blank
            && let Some(denotes) = self.lookup(name, true)
        {
            self.body.keys.insert(key, denotes);
        }
    }

    /// `x.sel`: a name of an imported package when `x` is an import.
    fn selector(&mut self, id: ExprId, x: ExprId, sel: Ident) {
        let file = self.file;
        let import = match file.exprs[x].kind {
            ExprKind::Name(name) => self.import_named(name),
            _ => None,
        };
        let Some(import) = import else {
            self.expr(x);
            return;
        };
        self.body.uses.insert(x, Ref::Object(import));
        let ObjKind::Import { package, .. } = &self.cx.package.object(import).kind else {
            return;
        };
        if package.fake || self.lenient {
            return;
        }
        let member = self.name_text(sel.name);
        match package.scope.get(&sel.name) {
            Some(&found) if is_exported(member) => {
                self.body.uses.insert(
                    id,
                    Ref::Member {
                        import,
                        member: found,
                    },
                );
            }
            Some(_) => {
                let message = format!("name {member} not exported by package {}", package.name);
                self.error(Code::UnexportedName, sel.span, message);
            }
            None => {
                let written = &self.text[file.exprs[x].span.lo as usize..sel.span.hi as usize];
                // A name that differs only in its capitals is the likely one.
                let near = package.objects.iter().find_map(|object| {
                    let name = self.cx.names.resolve(&object.name);
                    let near = is_exported(name)
                        && name.to_lowercase() == member.to_lowercase()
                        && package.scope.contains_key(&object.name);
                    near.then_some(name)
                });
                let message = match near {
                    Some(near) => format!("undefined: {written} (but have {near})"),
                    None => format!("undefined: {written}"),
                };
                self.error(Code::Undefined, sel.span, message);
            }
        }
    }

    // ---- statements

    fn stmts(&mut self, stmts: &[StmtId]) {
        for &stmt in stmts {
            self.stmt(stmt);
        }
    }

    fn opt_stmt(&mut self, stmt: Option<StmtId>) {
        if let Some(stmt) = stmt {
            self.stmt(stmt);
        }
    }

    fn block(&mut self, block: &Block) {
        self.open();
        self.stmts(&block.stmts);
        self.close();
    }

    fn stmt(&mut self, id: StmtId) {
        with_stack(|| self.stmt_node(id));
    }

    fn stmt_node(&mut self, id: StmtId) {
        let file = self.file;
        self.steps += 1;
        match &file.stmts[id].kind {
            StmtKind::Bad | StmtKind::Empty | StmtKind::Branch { .. } => {}
            StmtKind::Labeled { stmt, .. } => self.stmt(*stmt),
            StmtKind::Block(block) => self.block(block),
            StmtKind::Expr(x)
            | StmtKind::IncDec { x, .. }
            | StmtKind::Go(x)
            | StmtKind::Defer(x) => self.expr(*x),
            StmtKind::Send { chan, value } => {
                self.expr(*chan);
                self.expr(*value);
            }
            StmtKind::Decl(decls) => {
                for &decl in decls {
                    self.local_decl(decl);
                }
            }
            StmtKind::Assign {
                op: AssignOp::Define,
                lhs,
                rhs,
            } => {
                let errors = self.diagnostics.len();
                self.exprs(rhs);
                self.define(lhs, self.diagnostics.len() > errors);
            }
            StmtKind::Assign {
                op: AssignOp::Assign,
                lhs,
                rhs,
            } => {
                for &target in lhs {
                    self.target(target);
                }
                self.exprs(rhs);
            }
            StmtKind::Assign { lhs, rhs, .. } => {
                self.exprs(lhs);
                self.exprs(rhs);
            }
            StmtKind::Return(results) => {
                self.exprs(results);
                // `return` alone returns the results by their names, which
                // must still denote them here.
                let hidden: Vec<(LocalId, LocalId)> = match (&results[..], self.results.last()) {
                    ([], Some(named)) => named
                        .iter()
                        .filter_map(|&(name, result)| {
                            let now = self.visible.get(&name)?.local;
                            (now != result).then_some((result, now))
                        })
                        .collect(),
                    _ => Vec::new(),
                };
                if !hidden.is_empty() {
                    self.body.hidden_results.insert(id, hidden);
                }
            }
            StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            } => {
                self.open();
                self.opt_stmt(*init);
                self.expr(*cond);
                self.block(then);
                self.opt_stmt(*otherwise);
                self.close();
            }
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => {
                self.open();
                let before = self.body.locals.len();
                self.opt_stmt(*init);
                let declared = self.body.locals.len();
                self.opt_expr(*cond);
                // A post statement that declares is an error of its own:
                // its variables are not also unused.
                match post.map(|post| &file.stmts[post].kind) {
                    Some(StmtKind::Assign {
                        op: AssignOp::Define,
                        lhs,
                        rhs,
                    }) => {
                        self.exprs(rhs);
                        self.define(lhs, true);
                    }
                    _ => self.opt_stmt(*post),
                }
                self.loop_body(before..declared, body);
                self.close();
            }
            StmtKind::Range {
                lhs,
                define,
                x,
                body,
            } => {
                self.open();
                // The range expression does not see the loop variables.
                self.expr(*x);
                let before = self.body.locals.len();
                for &target in lhs {
                    if *define {
                        self.loop_var(target);
                    } else {
                        self.target(target);
                    }
                }
                // `for _ := range x` declares nothing.
                let blank = self.cx.blank;
                let is_blank = |&target: &ExprId| matches!(file.exprs[target].kind, ExprKind::Name(name) if name == blank);
                if let (true, Some(&last)) = (*define && lhs.iter().all(is_blank), lhs.last()) {
                    let after = file.exprs[last].span.hi as usize;
                    let operator =
                        self.text[after..].find(":=").map_or(after, |at| after + at) as u32;
                    let span = Span {
                        lo: operator,
                        hi: operator + 2,
                        ..file.exprs[last].span
                    };
                    let message = "no new variables on left side of :=";
                    self.error(Code::DefineNoNewVars, span, message);
                }
                self.loop_body(before..self.body.locals.len(), body);
                self.close();
            }
            StmtKind::Switch { init, tag, clauses } => {
                self.open();
                self.opt_stmt(*init);
                let guard = tag.map(|tag| &file.exprs[tag].kind);
                if let Some(&ExprKind::TypeSwitchGuard { lhs, x }) = guard {
                    self.expr(x);
                    self.type_switch(id, lhs, clauses);
                } else {
                    self.opt_expr(*tag);
                    for clause in clauses {
                        self.exprs(clause.cases.as_deref().unwrap_or(&[]));
                        self.open();
                        self.stmts(&clause.body);
                        self.close();
                    }
                }
                self.close();
            }
            StmtKind::Select { clauses } => {
                for clause in clauses {
                    if let Some(comm) = clause.comm.filter(|&comm| !self.communicates(comm)) {
                        // Nothing of the clause is looked at.
                        let message =
                            "select case must be send or receive (possibly with assignment)";
                        self.error(Code::SelectCaseInvalid, file.stmts[comm].span, message);
                        continue;
                    }
                    self.open();
                    self.opt_stmt(clause.comm);
                    self.stmts(&clause.body);
                    self.close();
                }
            }
        }
    }

    /// Whether the statement sends, or receives: `<-ch`, `v = <-ch`,
    /// `v, ok := <-ch`.
    fn communicates(&self, comm: StmtId) -> bool {
        let file = self.file;
        let received = match &file.stmts[comm].kind {
            StmtKind::Send { .. } => return true,
            StmtKind::Expr(x) => *x,
            StmtKind::Assign { rhs, .. } if rhs.len() == 1 => rhs[0],
            _ => return false,
        };
        let mut inner = received;
        while let ExprKind::Paren(x) = file.exprs[inner].kind {
            inner = x;
        }
        matches!(
            file.exprs[inner].kind,
            ExprKind::Unary {
                op: UnaryOp::Recv,
                ..
            }
        )
    }

    /// The clauses of a type switch. Its variable is declared anew in each
    /// clause, and is used if any clause uses it.
    fn type_switch(&mut self, id: StmtId, lhs: Option<Ident>, clauses: &[CaseClause]) {
        let lhs = lhs.filter(|lhs| {
            let named = lhs.name != self.cx.blank;
            if !named {
                let message = "no new variable on left side of :=";
                self.error(Code::DefineNoNewVars, lhs.span, message);
            }
            named
        });
        let mut vars = Vec::new();
        for clause in clauses {
            self.exprs(clause.cases.as_deref().unwrap_or(&[]));
            self.open();
            if let Some(lhs) = lhs {
                let local = self.local(lhs, LocalKind::Var);
                self.bind(lhs.name, local);
                vars.push(local);
            }
            self.stmts(&clause.body);
            self.close();
        }
        let Some(lhs) = lhs else { return };
        let mut used = false;
        for &var in &vars {
            let local = &mut self.body.locals[var.0 as usize];
            used |= std::mem::replace(&mut local.used, true);
        }
        if !used {
            let message = format!("{} declared and not used", self.name_text(lhs.name));
            self.error(Code::DeclaredNotUsed, lhs.span, message);
        }
        self.body.switch_vars.insert(id, vars);
    }

    /// The left side of `:=`: a name that the innermost scope already has
    /// is assigned to, every other name is a new variable.
    fn define(&mut self, lhs: &[ExprId], in_error: bool) {
        let file = self.file;
        let mut new = Vec::new();
        let mut repeated = false;
        for &target in lhs {
            let node = &file.exprs[target];
            let ExprKind::Name(name) = node.kind else {
                self.expr(target);
                continue;
            };
            if name == self.cx.blank {
                continue;
            }
            if !self.seen.insert(name) {
                let message = format!("{} repeated on left side of :=", self.name_text(name));
                self.error(Code::Redeclared, node.span, message);
                repeated = true;
                continue;
            }
            self.steps += 1;
            match self.visible.get(&name) {
                Some(binding) if binding.scope == self.scope() => {
                    self.body.uses.insert(target, Ref::Local(binding.local));
                    // What is no variable is not assigned to, which is
                    // said once the types are known.
                    let kind = self.body.locals[binding.local.0 as usize].kind;
                    repeated |= matches!(
                        kind,
                        LocalKind::Const | LocalKind::Type | LocalKind::TypeParam
                    );
                }
                _ => new.push(Ident {
                    name,
                    span: node.span,
                }),
            }
        }
        self.seen.clear();
        if new.is_empty()
            && !repeated
            && let Some(&last) = lhs.last()
        {
            let after = file.exprs[last].span.hi as usize;
            let operator = self.text[after..].find(":=").map_or(after, |at| after + at) as u32;
            let span = Span {
                lo: operator,
                hi: operator + 2,
                ..file.exprs[last].span
            };
            let message = "no new variables on left side of :=";
            self.error(Code::DefineNoNewVars, span, message);
        }
        for ident in new {
            self.declare_valued(ident, LocalKind::Var, in_error);
        }
    }

    /// A variable of `for … := range`: a name, or `&name` for an alias.
    fn loop_var(&mut self, target: ExprId) {
        let file = self.file;
        let named = match file.exprs[target].kind {
            ExprKind::Unary {
                op: UnaryOp::Addr,
                x,
            } => x,
            _ => target,
        };
        let node = &file.exprs[named];
        match node.kind {
            ExprKind::Name(name) => {
                let ident = Ident {
                    name,
                    span: node.span,
                };
                self.declare(ident, LocalKind::Var);
            }
            _ => self.expr(target),
        }
    }

    /// What `=` assigns to. Assigning to a variable does not use it.
    fn target(&mut self, target: ExprId) {
        let file = self.file;
        let mut inner = target;
        while let ExprKind::Paren(x) = file.exprs[inner].kind {
            inner = x;
        }
        match file.exprs[inner].kind {
            ExprKind::Name(name) if name == self.cx.blank => {}
            // In a `.gopp` file an assignment to a pointer writes what it
            // points to, which reads the pointer. Only the types tell the
            // two apart, so there the assignment counts as a use.
            ExprKind::Name(name) => self.name(inner, name, self.dialect == Dialect::Gopp),
            _ => self.expr(target),
        }
    }
}
