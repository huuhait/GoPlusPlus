//! One function: its locals as variables of C, its blocks as labels,
//! and the ends of its blocks as jumps.

use std::fmt::Write;

use gopp_gir::{
    AggKind, AssertKind, BinOp, BlockId, CastKind, Constant, ConstantKind, Function, Inline,
    LocalId, Operand, Place, ProjectionKind, Rvalue, StatementKind, TerminatorKind, Type, TypeId,
    UnOp, UnwindAction,
};

use gopp_target::{Header, SLICE, STRING};

use super::number::{float_literal, int_literal, int_tag};
use super::{Cx, EmitError, global_name, linkage, quoted};

type Written<T = ()> = Result<T, EmitError>;

fn local_name(local: LocalId) -> String {
    format!("_{}", local.into_raw().into_u32())
}

/// The name of the part `at` of a type whose parts have names in C.
fn member(names: &[&'static str], at: u32) -> &'static str {
    names.get(at as usize).copied().unwrap_or_default()
}

fn header_field(header: &Header, at: u32) -> &'static str {
    let field = header.fields.get(at as usize);
    field.map_or("", |&(name, _)| name)
}

fn block_name(block: BlockId) -> String {
    format!("bb{}", block.into_raw().into_u32())
}

/// The first line of the function: its attributes, its result, its name
/// and its parameters. A declaration also says the symbol.
pub(super) fn prototype(
    cx: &Cx<'_>,
    at: usize,
    func: &Function,
    declares: bool,
) -> Written<String> {
    let Type::Func(sig) = cx.kind(func.sig()) else {
        let what = "a function without a function type";
        return Err(unsupported(func, what));
    };
    let mut line = String::new();
    if func.no_return() {
        line.push_str("[[noreturn]] ");
    }
    if func.no_unwind() {
        line.push_str("__attribute__((nothrow)) ");
    }
    match func.inline() {
        Inline::Default => {}
        Inline::Always => line.push_str("__attribute__((always_inline)) inline "),
        Inline::Never => line.push_str("__attribute__((noinline)) "),
    }
    line.push_str(linkage(func.link()));
    let go_call = cx.go_call(sig);
    if go_call {
        line.push_str("GOPP_CALL ");
    }
    let _ = write!(line, "{} {}(", cx.spelled(sig.ret), cx.func_name(at));
    let mut first = true;
    for (number, &param) in sig.params.iter().enumerate() {
        if cx.empty(param) {
            continue;
        }
        if !std::mem::replace(&mut first, false) {
            line.push_str(", ");
        }
        if go_call && sig.has_env && number == 0 {
            line.push_str("GOPP_ENV ");
        }
        let _ = write!(line, "{} _{}", cx.spelled(param), number + 1);
    }
    if first {
        line.push_str("void");
    }
    line.push(')');
    if declares {
        let _ = write!(line, " __asm__(\"{}\")", quoted(func.name().as_bytes()));
    }
    Ok(line)
}

fn unsupported(func: &Function, what: &'static str) -> EmitError {
    EmitError::Unsupported {
        func: Some(func.name().to_owned()),
        what,
    }
}

/// The text of one function, and the steps that writing it took.
pub(super) fn function(cx: &Cx<'_>, at: usize, func: &Function) -> Written<(String, u64)> {
    let mut writer = Writer {
        cx,
        func,
        out: String::new(),
        steps: 0,
    };
    let Type::Func(sig) = cx.kind(func.sig()) else {
        return Err(unsupported(func, "a function without a function type"));
    };
    let _ = writeln!(writer.out, "\n{} {{", prototype(cx, at, func, false)?);
    for (number, (id, local)) in func.locals().iter().enumerate() {
        writer.steps += 1;
        let param = (1..=sig.params.len()).contains(&number);
        if !param && !cx.empty(local.ty) {
            let _ = writeln!(
                writer.out,
                "  [[maybe_unused]] {} {};",
                cx.spelled(local.ty),
                local_name(id)
            );
        }
    }
    // A label for each block that a jump names.
    let mut targeted = vec![false; func.blocks().len()];
    for (_, block) in func.blocks().iter() {
        for (target, _) in block.term.kind.successors() {
            targeted[target.into_raw().into_u32() as usize] = true;
        }
    }
    for (number, (id, block)) in func.blocks().iter().enumerate() {
        if block.cleanup || block.catch {
            return Err(unsupported(func, "a cleanup block"));
        }
        if targeted[number] {
            let _ = writeln!(writer.out, "{}:", block_name(id));
        }
        for stmt in &block.stmts {
            writer.steps += 1;
            if let StatementKind::Assign(assigned) = &stmt.kind {
                let (dest, value) = &**assigned;
                writer.assign(dest, value)?;
            }
        }
        writer.steps += 1;
        writer.end(&block.term.kind)?;
    }
    writer.out.push_str("}\n");
    Ok((writer.out, writer.steps))
}

struct Writer<'c, 'm> {
    cx: &'c Cx<'m>,
    func: &'c Function,
    out: String,
    steps: u64,
}

impl Writer<'_, '_> {
    fn place_ty(&self, place: &Place) -> TypeId {
        match place.projections.last() {
            Some(last) => last.ty,
            None => self.func.locals()[place.local].ty,
        }
    }

    fn operand_ty(&self, operand: &Operand) -> TypeId {
        match operand {
            Operand::Copy(place) | Operand::Move(place) => self.place_ty(place),
            Operand::Const(constant) => constant.ty,
        }
    }

    /// The place as an expression of C that can be assigned to.
    fn place(&mut self, place: &Place) -> Written<String> {
        let mut text = local_name(place.local);
        let mut ty = self.func.locals()[place.local].ty;
        for step in &place.projections {
            self.steps += 1;
            text = match (step.kind, self.cx.kind(ty)) {
                (ProjectionKind::Deref, _) => format!("(*{text})"),
                (ProjectionKind::Field(at), Type::String) => {
                    format!("{text}.{}", header_field(&STRING, at))
                }
                (ProjectionKind::Field(at), Type::Slice(_)) => {
                    format!("{text}.{}", header_field(&SLICE, at))
                }
                (ProjectionKind::Field(at), Type::Option(_)) => {
                    format!("{text}.{}", member(&["value", "set"], at))
                }
                (ProjectionKind::Field(at), _) => format!("{text}.f{at}"),
                (ProjectionKind::Index(at), Type::Array(..)) => {
                    format!("{text}.a[{}]", local_name(at))
                }
                (ProjectionKind::Index(at), _) => format!("{text}.ptr[{}]", local_name(at)),
                (ProjectionKind::ConstIndex(at), _) => format!("{text}.a[{at}]"),
                (ProjectionKind::Downcast(_), _) => {
                    return Err(unsupported(self.func, "a variant of a value"));
                }
            };
            ty = step.ty;
        }
        Ok(text)
    }

    fn constant(&mut self, constant: &Constant) -> Written<String> {
        let (cx, ty) = (self.cx, constant.ty);
        Ok(match (constant.kind, cx.kind(ty)) {
            (ConstantKind::Int(value), Type::Int { bits, signed }) => {
                int_literal(value, *bits, *signed)
            }
            (ConstantKind::Float(bits), Type::Float { bits: width }) => {
                float_literal(bits, *width == 32)
            }
            (ConstantKind::Bool(value), _) => value.to_string(),
            (ConstantKind::Str(at), _) => {
                let len = cx
                    .module
                    .strings
                    .get(at as usize)
                    .map_or(0, |text| text.len());
                format!("(({}){{(uint8_t *)gopp_s{at}, {len}, 0}})", cx.spelled(ty))
            }
            (ConstantKind::Func(func), _) => {
                let at = func.into_raw().into_u32() as usize;
                self.cx.func_name(at).to_owned()
            }
            (ConstantKind::Global(global), _) => {
                let at = global.into_raw().into_u32() as usize;
                match cx.empty_global(at) {
                    true => format!("(({})&gopp_empty)", cx.spelled(ty)),
                    false => format!("(&{})", global_name(at)),
                }
            }
            (ConstantKind::Null, _) => format!("(({})0)", cx.spelled(ty)),
            (ConstantKind::Zero, Type::Bool) => "false".to_owned(),
            (ConstantKind::Zero, Type::Int { bits, signed }) => int_literal(0, *bits, *signed),
            (ConstantKind::Zero, Type::Float { bits }) => float_literal(0, *bits == 32),
            (ConstantKind::Zero, Type::Ptr(_) | Type::Func(_)) => {
                format!("(({})0)", cx.spelled(ty))
            }
            (ConstantKind::Zero, _) => format!("(({}){{}})", cx.spelled(ty)),
            (ConstantKind::Int(_) | ConstantKind::Float(_), _) => {
                return Err(unsupported(
                    self.func,
                    "a number of a type that is no number",
                ));
            }
        })
    }

    fn operand(&mut self, operand: &Operand) -> Written<String> {
        self.steps += 1;
        match operand {
            Operand::Copy(place) | Operand::Move(place) => self.place(place),
            Operand::Const(constant) => self.constant(constant),
        }
    }

    /// `dest = value`; nothing for a value that takes no room.
    fn assign(&mut self, dest: &Place, value: &Rvalue) -> Written {
        let ty = self.place_ty(dest);
        if self.cx.empty(ty) {
            return Ok(());
        }
        let (dest, value) = (self.place(dest)?, self.rvalue(value, ty)?);
        let _ = writeln!(self.out, "  {dest} = {value};");
        Ok(())
    }

    /// The value as an expression of C of the type `ty`.
    fn rvalue(&mut self, value: &Rvalue, ty: TypeId) -> Written<String> {
        let cx = self.cx;
        Ok(match value {
            Rvalue::Use(operand) => self.operand(operand)?,
            Rvalue::AddrOf(place, _) => match cx.empty(self.place_ty(place)) {
                true => format!("(({})&gopp_empty)", cx.spelled(ty)),
                false => format!("(&{})", self.place(place)?),
            },
            Rvalue::Len(place) => match cx.kind(self.place_ty(place)) {
                Type::Array(_, count) => int_literal(*count, 64, false),
                _ => format!("{}.len", self.place(place)?),
            },
            Rvalue::Unary(op, operand) => {
                let (of, text) = (self.operand_ty(operand), self.operand(operand)?);
                match (op, cx.kind(of)) {
                    (UnOp::Not, _) => format!("(!{text})"),
                    (UnOp::Neg, Type::Float { .. }) => format!("(-{text})"),
                    (UnOp::Neg, _) => format!("(({})(-{text}))", cx.spelled(of)),
                    (UnOp::BitNot, _) => format!("(({})(~{text}))", cx.spelled(of)),
                }
            }
            Rvalue::Binary(op, first, second) => self.binary(*op, first, second)?,
            Rvalue::Cast(kind, operand, to) => {
                let (from, text) = (self.operand_ty(operand), self.operand(operand)?);
                let spelled = cx.spelled(*to);
                match (kind, cx.kind(from), cx.kind(*to)) {
                    (
                        CastKind::FloatToInt,
                        Type::Float { bits: float },
                        Type::Int { bits, signed },
                    ) => format!("gopp_f{float}_to_{}({text})", int_tag(*bits, *signed)),
                    (CastKind::PtrToInt | CastKind::IntToPtr, ..) => {
                        format!("(({spelled})(uintptr_t){text})")
                    }
                    _ => format!("(({spelled}){text})"),
                }
            }
            Rvalue::Aggregate(kind, parts) => {
                let mut written = Vec::with_capacity(parts.len());
                for part in parts {
                    if !cx.empty(self.operand_ty(part)) {
                        written.push(self.operand(part)?);
                    }
                }
                let inside = written.join(", ");
                match kind {
                    AggKind::Array(_) => format!("(({}){{{{{inside}}}}})", cx.spelled(ty)),
                    AggKind::Tuple | AggKind::Of(_) => {
                        format!("(({}){{{inside}}})", cx.spelled(ty))
                    }
                }
            }
        })
    }

    fn binary(&mut self, op: BinOp, first: &Operand, second: &Operand) -> Written<String> {
        let cx = self.cx;
        let ty = self.operand_ty(first);
        let (a, b) = (self.operand(first)?, self.operand(second)?);
        let spelled = cx.spelled(ty);
        let compared = |sign: &str| format!("({a} {sign} {b})");
        Ok(match (op, cx.kind(ty)) {
            (BinOp::Eq, _) => compared("=="),
            (BinOp::Ne, _) => compared("!="),
            (BinOp::Lt, _) => compared("<"),
            (BinOp::Le, _) => compared("<="),
            (BinOp::Gt, _) => compared(">"),
            (BinOp::Ge, _) => compared(">="),
            (BinOp::Shl, Type::Int { bits, signed }) => {
                format!("gopp_shl_{}({a}, (uint64_t){b})", int_tag(*bits, *signed))
            }
            (BinOp::Shr, Type::Int { bits, signed }) => {
                format!("gopp_shr_{}({a}, (uint64_t){b})", int_tag(*bits, *signed))
            }
            (BinOp::Div, Type::Int { bits, signed: true }) => {
                format!("gopp_div_{}({a}, {b})", int_tag(*bits, true))
            }
            (BinOp::Rem, Type::Int { bits, signed: true }) => {
                format!("gopp_rem_{}({a}, {b})", int_tag(*bits, true))
            }
            (BinOp::BitClear, _) => format!("(({spelled})({a} & ~{b}))"),
            (_, Type::Float { .. }) => {
                let sign = match op {
                    BinOp::Add => "+",
                    BinOp::Sub => "-",
                    BinOp::Mul => "*",
                    _ => "/",
                };
                format!("({a} {sign} {b})")
            }
            _ => {
                let sign = match op {
                    BinOp::Add => "+",
                    BinOp::Sub => "-",
                    BinOp::Mul => "*",
                    BinOp::Div => "/",
                    BinOp::Rem => "%",
                    BinOp::BitAnd => "&",
                    BinOp::BitOr => "|",
                    _ => "^",
                };
                format!("(({spelled})({a} {sign} {b}))")
            }
        })
    }

    fn jump(&mut self, to: BlockId) {
        let _ = writeln!(self.out, "  goto {};", block_name(to));
    }

    /// What unwinding does needs no code of its own, but for a block
    /// that it runs.
    fn unwind(&self, action: UnwindAction) -> Written {
        match action {
            UnwindAction::Cleanup(_) => Err(unsupported(self.func, "a cleanup block")),
            _ => Ok(()),
        }
    }

    /// The end of a block.
    fn end(&mut self, term: &TerminatorKind) -> Written {
        let cx = self.cx;
        match term {
            TerminatorKind::Goto(target) => self.jump(*target),
            TerminatorKind::Return => {
                let result = self.func.locals().iter().next();
                match result.filter(|(_, result)| !cx.empty(result.ty)) {
                    Some((id, _)) => {
                        let _ = writeln!(self.out, "  return {};", local_name(id));
                    }
                    None => self.out.push_str("  return;\n"),
                }
            }
            TerminatorKind::Unreachable => self.out.push_str("  __builtin_unreachable();\n"),
            TerminatorKind::Switch {
                on,
                cases,
                otherwise,
            } => {
                let (ty, text) = (self.operand_ty(on), self.operand(on)?);
                match cx.kind(ty) {
                    Type::Int { bits, signed } => {
                        let _ = writeln!(self.out, "  switch ({text}) {{");
                        for &(value, target) in cases {
                            self.steps += 1;
                            let _ = writeln!(
                                self.out,
                                "  case {}: goto {};",
                                int_literal(value, *bits, *signed),
                                block_name(target)
                            );
                        }
                        let _ = writeln!(
                            self.out,
                            "  default: goto {};\n  }}",
                            block_name(*otherwise)
                        );
                    }
                    _ => {
                        for &(value, target) in cases {
                            let holds = if value == 0 { "!" } else { "" };
                            let _ = writeln!(
                                self.out,
                                "  if ({holds}{text}) goto {};",
                                block_name(target)
                            );
                        }
                        self.jump(*otherwise);
                    }
                }
            }
            TerminatorKind::Call {
                callee,
                args,
                dest,
                target,
                unwind,
            } => {
                self.unwind(*unwind)?;
                let called = self.operand(callee)?;
                let mut given = Vec::with_capacity(args.len());
                for arg in args {
                    if !cx.empty(self.operand_ty(arg)) {
                        given.push(self.operand(arg)?);
                    }
                }
                let call = format!("{called}({})", given.join(", "));
                match cx.empty(self.place_ty(dest)) {
                    true => {
                        let _ = writeln!(self.out, "  {call};");
                    }
                    false => {
                        let dest = self.place(dest)?;
                        let _ = writeln!(self.out, "  {dest} = {call};");
                    }
                }
                match target {
                    Some(target) => self.jump(*target),
                    None => self.out.push_str("  __builtin_unreachable();\n"),
                }
            }
            TerminatorKind::Assert {
                cond,
                expected,
                kind,
                args,
                target,
                unwind,
            } => {
                self.unwind(*unwind)?;
                let holds = self.operand(cond)?;
                let fails = if *expected { "!" } else { "" };
                let mut said = Vec::with_capacity(args.len());
                for arg in args {
                    said.push(format!("(uint64_t){}", self.operand(arg)?));
                }
                // The message of Go has a negative index as it is.
                if let Some(first) = args.first() {
                    let ty = self.operand_ty(first);
                    let signed = matches!(self.cx.kind(ty), Type::Int { signed: true, .. });
                    said.push(signed.to_string());
                }
                let _ = writeln!(
                    self.out,
                    "  if (__builtin_expect({fails}{holds}, 0)) _gopp_panic_{}({});",
                    panic_name(*kind),
                    said.join(", ")
                );
                self.jump(*target);
            }
            TerminatorKind::Drop { .. } => return Err(unsupported(self.func, "a drop")),
            TerminatorKind::Resume => return Err(unsupported(self.func, "a cleanup block")),
        }
        Ok(())
    }
}

/// The function of the runtime that reports a check that failed.
fn panic_name(kind: AssertKind) -> &'static str {
    match kind {
        AssertKind::Bounds => "bounds",
        AssertKind::SliceAlen => "slice_alen",
        AssertKind::SliceAcap => "slice_acap",
        AssertKind::SliceB => "slice_b",
        AssertKind::Slice3Alen => "slice3_alen",
        AssertKind::Slice3Acap => "slice3_acap",
        AssertKind::Slice3B => "slice3_b",
        AssertKind::Slice3C => "slice3_c",
        AssertKind::SliceConvert => "slice_convert",
        AssertKind::Nil => "nil",
        AssertKind::DivByZero => "divide",
        AssertKind::NegativeShift => "shift",
    }
}
