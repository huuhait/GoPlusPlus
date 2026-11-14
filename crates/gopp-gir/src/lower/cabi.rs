//! Functions of C (c-abi.md): the type that a function has for C, what
//! a package declares for C, and the functions of C that call a
//! function of Go++: the one that an `@Export("C")` function is known
//! to C by, and the one that C code is given for a function (`_GPTC…`).

use gopp_diag::{Code, Diagnostic};
use gopp_mangle::{Entity, is_reserved_name, symbol_of};
use gopp_sema::basic::Basic;
use gopp_sema::types::{Marks, Type as CheckedType, TypeId as CheckedTypeId};
use gopp_sema::{ObjId, ObjKind, Ref};
use gopp_syntax::ast::ExprId;

use super::func::{Built, FuncCx, lower_forward};
use super::{CalleeKey, Made, Memo, Shared};
use crate::{
    CRefusal, CallConv, Constant, ConstantKind, FuncType, Linkage, Module, Operand, Rvalue, Type,
    TypeId, TypeInterner,
};

/// What the name of every function of the runtime starts with.
const RUNTIME_PREFIX: &str = "_gopp_";

/// Why a function has no type for C.
pub(super) struct Refused {
    /// What is declared cannot be C; otherwise it is not supported yet.
    wrong: bool,
    pub what: String,
}

impl Refused {
    fn wrong(what: String) -> Self {
        Self { wrong: true, what }
    }

    fn later(what: &str) -> Self {
        let what = what.to_owned();
        Self { wrong: false, what }
    }

    /// The same for the part of a function that `part` names.
    fn of(self, part: &str) -> Self {
        let what = format!("{part}: {}", self.what);
        Self { what, ..self }
    }
}

impl Memo<'_> {
    /// The first part of the type that C has no type for, among the
    /// parts that GIR does not tell from a type of C: a handle is a
    /// pointer there, and a function value two words.
    fn not_c(&mut self, shared: &Shared<'_>, ty: CheckedTypeId) -> Option<CheckedTypeId> {
        if let Some(&known) = self.c_parts.get(&ty) {
            return known;
        }
        self.steps += 1;
        let types = shared.types;
        let found = match &*types.get(ty) {
            _ if types.is_unsafe_pointer(ty, shared.names) => None,
            CheckedType::Basic(basic) => match basic.default_type() {
                Basic::String | Basic::Complex64 | Basic::Complex128 => Some(ty),
                _ => None,
            },
            CheckedType::Pointer(_) => None,
            CheckedType::Option(of) => match &*types.get(types.underlying(*of)) {
                CheckedType::Pointer(_) => None,
                CheckedType::Func { .. } => Some(*of),
                _ => Some(ty),
            },
            CheckedType::Array { elem, .. } => self.not_c(shared, *elem),
            CheckedType::Local(of) => self.not_c(shared, *of),
            CheckedType::Struct(fields) => {
                fields.iter().find_map(|field| self.not_c(shared, field.ty))
            }
            CheckedType::Named { .. } => {
                let mut parts = types.parents(ty);
                parts.push(types.underlying(ty));
                parts.into_iter().find_map(|part| self.not_c(shared, part))
            }
            _ => Some(ty),
        };
        self.c_parts.insert(ty, found);
        found
    }

    /// The type of GIR of a value of the type `ty` that a function of
    /// C takes or gives.
    fn c_value(
        &mut self,
        shared: &Shared<'_>,
        types: &mut TypeInterner,
        ty: CheckedTypeId,
    ) -> Result<TypeId, Refused> {
        let lowered = self.lower_type(shared, types, ty).map_err(Refused::later)?;
        let before = self.c_rules.steps;
        let crosses = self.c_rules.param(types, lowered);
        self.steps += self.c_rules.steps - before;
        // A type as deep as that is not walked again.
        if crosses != Err(CRefusal::Depth)
            && let Some(part) = self.not_c(shared, ty)
        {
            let is_func = matches!(
                &*shared.types.get(shared.types.underlying(part)),
                CheckedType::Func { .. }
            );
            return Err(match is_func {
                true => Refused::later("a function value inside a value that crosses to C"),
                false => {
                    let shown = shared.types.display(part, shared.names);
                    Refused::wrong(format!("C has no type for {shown}"))
                }
            });
        }
        crosses.map(|()| lowered).map_err(|why| {
            Refused::wrong(match why {
                // What is left is what takes no room.
                CRefusal::NotC(_) => "a value without a size has no one layout in C: a struct \
                                      without fields takes no room here and four bytes in the \
                                      C of Windows"
                    .to_owned(),
                other => other.describe(types),
            })
        })
    }

    /// The type that a function of the type `sig` has for C: it takes
    /// every parameter by its value, and a function, if `callbacks`
    /// allows one, as the address of code of C.
    pub(super) fn c_signature(
        &mut self,
        shared: &Shared<'_>,
        types: &mut TypeInterner,
        sig: CheckedTypeId,
        callbacks: bool,
    ) -> Result<TypeId, Refused> {
        let checked = shared.types;
        let is_func = |ty: CheckedTypeId| {
            matches!(
                &*checked.get(checked.underlying(ty)),
                CheckedType::Func { .. }
            )
        };
        let kind = checked.get(checked.underlying(sig));
        let CheckedType::Func {
            type_params,
            params,
            results,
            ..
        } = &*kind
        else {
            return Err(Refused::later("a function type"));
        };
        if !type_params.is_empty() {
            return Err(Refused::later("a generic function for C"));
        }
        let mut lowered = Vec::with_capacity(params.len());
        for (at, &param) in params.iter().enumerate() {
            let one = match (is_func(param), callbacks) {
                (true, true) => self.c_signature(shared, types, param, false),
                (true, false) => Err(Refused::later("a function that takes a function here")),
                (false, _) => self.c_value(shared, types, param),
            };
            lowered.push(one.map_err(|refused| refused.of(&format!("parameter {}", at + 1)))?);
        }
        let ret = match &results[..] {
            [] => TypeInterner::UNIT,
            [one] if is_func(*one) => {
                return Err(Refused::later("a function as what a function of C gives"));
            }
            [one] => self
                .c_value(shared, types, *one)
                .map_err(|refused| refused.of("the result"))?,
            _ => {
                let what = "the result: C gives one value".to_owned();
                return Err(Refused::wrong(what));
            }
        };
        Ok(types.intern(Type::Func(FuncType {
            params: lowered.into(),
            ret,
            has_env: false,
            cc: CallConv::C,
        })))
    }

    /// The type for C of a function of C of the type `sig` that a
    /// function of Go++ calls with what it is given, as it is.
    pub(super) fn c_plain_sig(
        &mut self,
        shared: &Shared<'_>,
        types: &mut TypeInterner,
        sig: CheckedTypeId,
    ) -> Result<TypeId, &'static str> {
        let what = "a function of C as a value whose type is not the same for Go++";
        self.c_thunk_sig(shared, types, sig).map_err(|_| what)
    }

    /// The type of the function of C that calls a function of Go++ of
    /// the type `sig`: it passes on what it is given, as it is.
    fn c_thunk_sig(
        &mut self,
        shared: &Shared<'_>,
        types: &mut TypeInterner,
        sig: CheckedTypeId,
    ) -> Result<TypeId, Refused> {
        let for_c = self.c_signature(shared, types, sig, false)?;
        let own = self
            .signature(shared, types, None, sig)
            .map_err(Refused::later)?;
        match (types.get(for_c), types.get(own)) {
            (Type::Func(for_c), Type::Func(own))
                if for_c.params == own.params && for_c.ret == own.ret => {}
            // The place of a view is a parameter that C does not pass.
            _ => {
                return Err(Refused::later(
                    "a function for C that gives a pointer to a slice",
                ));
            }
        }
        Ok(for_c)
    }
}

/// Whether C sees the name as one that Go++ makes.
fn reserved(name: &str) -> bool {
    is_reserved_name(name) || name.starts_with(RUNTIME_PREFIX)
}

/// Checks what the package declares for C, and adds the function of C
/// of each function that C calls to the module, under the name that
/// the function is declared with.
pub(super) fn lower_c_side<'a>(shared: &'a Shared<'a>, module: &mut Module, memo: &mut Memo<'a>) {
    let package = shared.package;
    for (at, object) in package.objects.iter().enumerate() {
        let in_c = object.marks.has(Marks::EXTERN);
        let for_c = object.marks.has(Marks::EXPORT);
        if !matches!(object.kind, ObjKind::Func) || in_c == for_c {
            continue;
        }
        let name = shared.names.resolve(&object.name);
        let wrong = match in_c {
            true => Code::GirExternBad,
            false => Code::GirExportBad,
        };
        let report = |memo: &mut Memo<'a>, code: Code, message: String| {
            memo.diagnostics
                .push(Diagnostic::new(code, object.span, message));
        };
        if reserved(name) {
            let message = format!(
                "C cannot know a function as {name}: the names that start with `_GP` or \
                 `{RUNTIME_PREFIX}` are those that Go++ makes"
            );
            report(memo, wrong, message);
            continue;
        }
        if for_c && name == "main" {
            let message = "C cannot call main: the program starts there".to_owned();
            report(memo, wrong, message);
            continue;
        }
        let sig = match in_c {
            true => memo.c_signature(shared, &mut module.types, object.ty, true),
            false => memo.c_thunk_sig(shared, &mut module.types, object.ty),
        };
        let sig = match sig {
            Ok(sig) => sig,
            Err(refused) => {
                let (code, message) = match refused.wrong {
                    true => (
                        wrong,
                        format!("{name} is no function of C: {}", refused.what),
                    ),
                    false => (
                        Code::GirUnsupported,
                        format!("not supported yet: {}", refused.what),
                    ),
                };
                report(memo, code, message);
                continue;
            }
        };
        let entity = Entity::Object(package, ObjId(at as u32));
        if let (true, Ok(of)) = (for_c, symbol_of(&mut memo.namer, entity)) {
            let link = Linkage::External;
            lower_forward(
                shared,
                module,
                memo,
                (name, sig, link),
                (&of, object.ty, false),
                0,
            );
        }
    }
}

/// Adds the function of C with the symbol that calls the function `of`
/// of the type `ty` to the module: every package that gives the
/// function to C has it, and the linker keeps one.
pub(super) fn lower_c_thunk<'a>(
    shared: &'a Shared<'a>,
    module: &mut Module,
    memo: &mut Memo<'a>,
    symbol: &str,
    (of, ty): (&str, CheckedTypeId),
) {
    // The one who asked for it knows that the function has the type.
    if let Ok(sig) = memo.c_thunk_sig(shared, &mut module.types, ty) {
        let link = Linkage::LinkOnceOdr;
        lower_forward(
            shared,
            module,
            memo,
            (symbol, sig, link),
            (of, ty, false),
            0,
        );
    }
}

impl FuncCx<'_, '_> {
    /// Whether a place of the type holds the address of code of C.
    pub(super) fn is_c_code(&mut self, ty: TypeId) -> bool {
        matches!(self.kind(ty), Type::Func(func) if func.cc == CallConv::C)
    }

    /// The address of code of C for a parameter of the type `ty` of a
    /// function of C: the function of C that the expression names, or
    /// the function of C that the package makes for a function of
    /// Go++. Any other function value has an environment, which C has
    /// no place for.
    pub(super) fn callback(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let named = Shared::unparen(self.tree, expr);
        let home = self.home();
        let found = match self.body.uses.get(&named) {
            Some(Ref::Object(object)) => Some((CalleeKey::Object(*object), home, *object)),
            Some(Ref::Member { import, member }) => match &home.object(*import).kind {
                ObjKind::Import { package, .. } => {
                    Some((CalleeKey::Member(*import, *member), &**package, *member))
                }
                _ => None,
            },
            _ => None,
        };
        let found = found.filter(|&(_, package, id)| {
            matches!(package.object(id).kind, ObjKind::Func) && self.instance_named(named).is_none()
        });
        let Some((key, package, id)) = found else {
            let what = "a function value for C that is no function of a package";
            return Ok(self.placeholder(span, ty, what));
        };
        let called = match self.callee(key)? {
            Ok(called) => called,
            Err(what) => return Ok(self.placeholder(span, ty, what)),
        };
        let code = |func| {
            let kind = ConstantKind::Func(func);
            Rvalue::Use(Operand::Const(Constant { kind, ty }))
        };
        let object = package.object(id);
        if object.marks.has(Marks::EXTERN) {
            return Ok(match called.sig == ty {
                true => code(called.func),
                false => self.placeholder(span, ty, "a function of C of another type"),
            });
        }
        let shared = self.shared;
        let sig = self.memo.c_thunk_sig(shared, self.b.types(), object.ty);
        match sig {
            Ok(sig) if sig == ty => {}
            Ok(_) => return Ok(self.placeholder(span, ty, "a function of another type for C")),
            Err(refused) => return Ok(self.placeholder(span, ty, &refused.what)),
        }
        let Ok(symbol) = symbol_of(&mut self.memo.namer, Entity::CabiThunk(package, id)) else {
            return Ok(self.placeholder(span, ty, "a function without a symbol for C"));
        };
        let asked = self.memo.funcs.contains_key(&symbol);
        let thunk = self.function_named(&symbol, ty)?;
        if !asked {
            let of = self.b.module().funcs[called.func].name().to_owned();
            self.memo.glues.push((symbol, Made::CThunk(of), object.ty));
        }
        Ok(code(thunk))
    }
}
