//! The text of a module (gir.md §12): the same for the same module,
//! whatever made it.

use std::fmt::Write;

use crate::ir::*;
use crate::types::{Type, TypeId};

/// The module as text.
pub fn print_module(module: &Module) -> String {
    let mut out = format!("module {:?}\n", module.name);
    for (at, file) in module.files.iter().enumerate() {
        let _ = writeln!(out, "file {at} {file:?}");
    }
    for (_, global) in module.globals.iter() {
        let kind = if global.mutable { "var" } else { "const" };
        let ty = module.types.display(global.ty);
        let _ = writeln!(
            out,
            "\nglobal {kind} @{}: {ty} [{}]",
            global.name,
            link(global.link)
        );
        for part in &global.parts {
            let _ = writeln!(out, "    {}", constant(module, part));
        }
    }
    for (_, func) in module.funcs.iter() {
        out.push('\n');
        Printer { module, func }.function(&mut out);
    }
    out
}

fn link(link: Linkage) -> &'static str {
    match link {
        Linkage::External => "external",
        Linkage::Internal => "internal",
        Linkage::LinkOnceOdr => "linkonce_odr",
    }
}

/// The bytes of a string constant: what is no printable ASCII as `\XX`.
fn quoted(bytes: &[u8]) -> String {
    let mut out = String::from("\"");
    for &byte in bytes {
        match byte {
            b'"' | b'\\' => {
                let _ = write!(out, "\\{byte:02X}");
            }
            0x20..=0x7e => out.push(byte as char),
            _ => {
                let _ = write!(out, "\\{byte:02X}");
            }
        }
    }
    out.push('"');
    out
}

/// A constant as the text of a module writes it.
fn constant(module: &Module, constant: &Constant) -> String {
    let ty = module.types.display(constant.ty);
    match constant.kind {
        ConstantKind::Int(bits) => {
            // A signed integer is written with its sign.
            let signed = match module.types.get(constant.ty) {
                Type::Int {
                    bits: width,
                    signed: true,
                } => {
                    let shift = 64 - u32::from(*width);
                    Some(((bits << shift) as i64) >> shift)
                }
                _ => None,
            };
            match signed {
                Some(value) => format!("const {value}: {ty}"),
                None => format!("const {bits}: {ty}"),
            }
        }
        ConstantKind::Float(bits) => format!("const 0x{bits:016x}: {ty}"),
        ConstantKind::Bool(value) => format!("const {value}"),
        ConstantKind::Str(at) => {
            let bytes = module.strings.get(at as usize);
            let text = bytes.map_or(String::new(), |bytes| quoted(bytes));
            format!("const str#{at} {text}")
        }
        ConstantKind::Func(func) => {
            format!("const fn @{}", module.funcs[func].name)
        }
        ConstantKind::Null => format!("const null: {ty}"),
        ConstantKind::Zero => format!("const zero: {ty}"),
        ConstantKind::Global(global) => {
            format!("const global @{}", module.globals[global].name)
        }
    }
}

struct Printer<'p> {
    module: &'p Module,
    func: &'p Function,
}

impl Printer<'_> {
    fn ty(&self, ty: TypeId) -> String {
        self.module.types.display(ty)
    }

    fn local(local: LocalId) -> String {
        format!("_{}", local.into_raw().into_u32())
    }

    fn block(block: BlockId) -> String {
        format!("bb{}", block.into_raw().into_u32())
    }

    /// `  ; file:line:col`, for a place in a file.
    fn loc(&self, loc: Span, scope: ScopeId) -> String {
        let file = loc
            .file
            .checked_sub(1)
            .and_then(|at| self.module.files.get(at as usize));
        let at = match file {
            Some(file) => {
                let name = file.rsplit(['/', '\\']).next().unwrap_or(file);
                format!("{name}:{}:{}", loc.line, loc.col)
            }
            None => String::new(),
        };
        match (at.is_empty(), scope.0) {
            (true, 0) => String::new(),
            (true, scope) => format!("  ; scope {scope}"),
            (false, 0) => format!("  ; {at}"),
            (false, scope) => format!("  ; {at} scope {scope}"),
        }
    }

    fn function(&self, out: &mut String) {
        let func = self.func;
        let (params, ret) = match self.module.types.get(func.sig) {
            Type::Func(sig) => (sig.params.to_vec(), sig.ret),
            _ => (Vec::new(), func.sig),
        };
        let params: Vec<String> = params
            .iter()
            .enumerate()
            .map(|(at, &ty)| format!("_{}: {}", at + 1, self.ty(ty)))
            .collect();
        let mut marks = vec![
            match func.cc {
                CallConv::Gopp => "gopp",
                CallConv::C => "c",
            },
            link(func.link),
        ];
        marks.extend(func.no_unwind.then_some("nounwind"));
        marks.extend(func.no_return.then_some("noreturn"));
        marks.extend(match func.inline {
            Inline::Default => None,
            Inline::Always => Some("inline"),
            Inline::Never => Some("noinline"),
        });
        let declared = if func.is_declaration() {
            "declare "
        } else {
            ""
        };
        let _ = write!(
            out,
            "{declared}fn {}({}) -> {} [{}]",
            func.name,
            params.join(", "),
            self.ty(ret),
            marks.join(", ")
        );
        if func.is_declaration() {
            out.push('\n');
            return;
        }
        out.push_str(" {\n");
        for (at, scope) in func.scopes.iter().enumerate() {
            let kind = match scope.kind {
                ScopeKind::Block => "block".to_owned(),
                ScopeKind::Inlined(callee) => {
                    format!("inlined @{}", self.module.funcs[callee].name)
                }
            };
            let line = format!("  scope {}: {kind} in {}", at + 1, scope.parent.0);
            let _ = writeln!(out, "{line}{}", self.loc(scope.loc, ScopeId(0)));
        }
        for (id, local) in func.locals.iter() {
            if local.kind == LocalKind::Arg && local.name.is_none() && !local.owned {
                continue;
            }
            let mut line = format!("  let {}: {}", Self::local(id), self.ty(local.ty));
            if let Some(name) = &local.name {
                let _ = write!(line, " {name:?}");
            }
            let marks: Vec<&str> = [
                (!local.mutable).then_some("const"),
                local.owned.then_some("owned"),
            ]
            .into_iter()
            .flatten()
            .collect();
            if !marks.is_empty() {
                let _ = write!(line, " [{}]", marks.join(", "));
            }
            let _ = writeln!(out, "{line}");
        }
        for (id, block) in func.blocks.iter() {
            let marks: Vec<&str> = [
                (block.cleanup && !block.catch).then_some("cleanup"),
                block.catch.then_some("catch"),
                block.cold.then_some("cold"),
            ]
            .into_iter()
            .flatten()
            .collect();
            let marks = match marks.is_empty() {
                true => String::new(),
                false => format!(" [{}]", marks.join(", ")),
            };
            let _ = writeln!(out, "  {}:{marks}", Self::block(id));
            for stmt in &block.stmts {
                let text = match &stmt.kind {
                    StatementKind::Assign(assigned) => {
                        let (dest, rvalue) = &**assigned;
                        format!("{} = {}", self.place(dest), self.rvalue(rvalue))
                    }
                    StatementKind::StorageLive(local) => {
                        format!("storage_live({})", Self::local(*local))
                    }
                    StatementKind::StorageDead(local) => {
                        format!("storage_dead({})", Self::local(*local))
                    }
                    StatementKind::Nop => "nop".to_owned(),
                };
                let _ = writeln!(out, "    {text}{}", self.loc(stmt.loc, stmt.scope));
            }
            let term = &block.term;
            let _ = writeln!(
                out,
                "    {}{}",
                self.terminator(&term.kind),
                self.loc(term.loc, term.scope)
            );
        }
        out.push_str("}\n");
    }

    fn place(&self, place: &Place) -> String {
        let mut text = Self::local(place.local);
        for step in &place.projections {
            text = match step.kind {
                ProjectionKind::Deref => format!("(*{text})"),
                ProjectionKind::Field(at) => format!("{text}.{at}"),
                ProjectionKind::Index(at) => format!("{text}[{}]", Self::local(at)),
                ProjectionKind::ConstIndex(at) => format!("{text}[{at}]"),
                ProjectionKind::Downcast(at) => format!("({text} as {at})"),
            };
        }
        text
    }

    fn operand(&self, operand: &Operand) -> String {
        match operand {
            Operand::Copy(place) => format!("copy {}", self.place(place)),
            Operand::Move(place) => format!("move {}", self.place(place)),
            Operand::Const(constant) => self::constant(self.module, constant),
        }
    }

    fn operands(&self, operands: &[Operand]) -> String {
        let all: Vec<String> = operands
            .iter()
            .map(|operand| self.operand(operand))
            .collect();
        all.join(", ")
    }

    fn rvalue(&self, rvalue: &Rvalue) -> String {
        match rvalue {
            Rvalue::Use(operand) => self.operand(operand),
            Rvalue::AddrOf(place, Mutability::Not) => format!("&{}", self.place(place)),
            Rvalue::AddrOf(place, Mutability::Mut) => format!("&mut {}", self.place(place)),
            Rvalue::Binary(op, left, right) => {
                let name = format!("{op:?}").to_lowercase();
                format!("{name}({}, {})", self.operand(left), self.operand(right))
            }
            Rvalue::Unary(op, operand) => {
                let name = format!("{op:?}").to_lowercase();
                format!("{name}({})", self.operand(operand))
            }
            Rvalue::Cast(kind, operand, to) => {
                let name = match kind {
                    CastKind::IntToInt => "int_to_int",
                    CastKind::IntToFloat => "int_to_float",
                    CastKind::FloatToInt => "float_to_int",
                    CastKind::FloatToFloat => "float_to_float",
                    CastKind::PtrToPtr => "ptr_to_ptr",
                    CastKind::PtrToInt => "ptr_to_int",
                    CastKind::IntToPtr => "int_to_ptr",
                };
                format!("{name}({}) as {}", self.operand(operand), self.ty(*to))
            }
            Rvalue::Aggregate(kind, operands) => {
                let what = match kind {
                    AggKind::Tuple => "tuple".to_owned(),
                    AggKind::Array(of) => format!("array {}", self.ty(*of)),
                    AggKind::Of(ty) => self.ty(*ty),
                };
                format!("{what} {{{}}}", self.operands(operands))
            }
            Rvalue::Len(place) => format!("len({})", self.place(place)),
        }
    }

    fn unwind(action: UnwindAction) -> String {
        match action {
            UnwindAction::Continue => "continue".to_owned(),
            UnwindAction::Cleanup(block) => format!("cleanup {}", Self::block(block)),
            UnwindAction::Terminate => "terminate".to_owned(),
            UnwindAction::Unreachable => "unreachable".to_owned(),
        }
    }

    fn terminator(&self, kind: &TerminatorKind) -> String {
        match kind {
            TerminatorKind::Goto(target) => format!("goto {}", Self::block(*target)),
            TerminatorKind::Switch {
                on,
                cases,
                otherwise,
            } => {
                let mut ways: Vec<String> = cases
                    .iter()
                    .map(|&(value, target)| format!("{value}: {}", Self::block(target)))
                    .collect();
                ways.push(format!("otherwise: {}", Self::block(*otherwise)));
                format!("switch {} [{}]", self.operand(on), ways.join(", "))
            }
            TerminatorKind::Return => "return".to_owned(),
            TerminatorKind::Unreachable => "unreachable".to_owned(),
            TerminatorKind::Resume => "resume".to_owned(),
            TerminatorKind::Call {
                callee,
                args,
                dest,
                target,
                unwind,
            } => {
                let back = match target {
                    Some(target) => format!("return: {}, ", Self::block(*target)),
                    None => String::new(),
                };
                format!(
                    "{} = call {}({}) -> [{back}unwind: {}]",
                    self.place(dest),
                    self.operand(callee),
                    self.operands(args),
                    Self::unwind(*unwind)
                )
            }
            TerminatorKind::Drop {
                place,
                target,
                unwind,
            } => format!(
                "drop({}) -> [return: {}, unwind: {}]",
                self.place(place),
                Self::block(*target),
                Self::unwind(*unwind)
            ),
            TerminatorKind::Assert {
                cond,
                expected,
                kind,
                args,
                target,
                unwind,
            } => {
                let mut said = format!("{kind:?}");
                if !args.is_empty() {
                    let _ = write!(said, "({})", self.operands(args));
                }
                format!(
                    "assert({} == {expected}, {said}) -> [success: {}, unwind: {}]",
                    self.operand(cond),
                    Self::block(*target),
                    Self::unwind(*unwind)
                )
            }
        }
    }
}
