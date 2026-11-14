//! `panic` and `recover` (PAN-1, PAN-5, gir.md §17). The runtime has
//! the value of a panic as an interface value, makes the deferred calls
//! of every function that has not returned, and goes on in the function
//! whose deferred call stopped the panic.

use gopp_sema::basic::Basic;
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::ExprId;
use lasso::Spur;

use super::func::{Built, FuncCx};
use crate::{Operand, Place, Rvalue, Type, TypeId};

impl FuncCx<'_, '_> {
    /// `panic(v)`: the runtime gets `v` in an interface value, which it
    /// owns, and the keys of the methods by which an error or a value
    /// with a `String` method says its text if the panic ends the
    /// program. Nothing after the call is reached.
    pub fn panic(&mut self, span: SourceSpan, value: ExprId) -> Built {
        let ty = self.lower(self.shared.types.any(), span);
        if !self.is_iface(ty) {
            self.unsupported(span, "a panic with this value");
            return Ok(());
        }
        let held = self.rvalue_as(value, ty)?;
        let held = self.taken(value, ty, held)?;
        let held = self.slot_of(held)?;
        self.empty_moved()?;
        self.panic_with(span, held)
    }

    /// A panic with the interface value in the place, which the
    /// runtime has from here on.
    pub fn panic_with(&mut self, span: SourceSpan, held: Place) -> Built {
        let (error, string) = (self.shared.error, self.shared.string);
        let keys = [self.text_key(error), self.text_key(string)];
        let bytes = self.bytes_ty();
        let text = self.b.types().intern(Type::String);
        let [error, string] = keys;
        let args = vec![self.record(held)?, error, string];
        let around = self.b.loc();
        self.at(span);
        self.runtime_call("_gopp_panic", &[bytes, text, text], args)?;
        self.b.unreachable()?;
        self.b.set_loc(around);
        self.dead();
        Ok(())
    }

    /// The key of the method `name` that takes nothing and gives a
    /// string, as the tables of methods have it.
    fn text_key(&mut self, name: Spur) -> Operand {
        let types = self.shared.types;
        let method = gopp_sema::types::Method {
            name,
            package: None,
            sig: types.func(&[], &[types.basic(Basic::String)], false),
        };
        // A type of two basic types has a symbol.
        let key = self.method_key(&method).unwrap_or_default();
        self.b.const_str(&key)
    }

    /// `recover()`: the value of the panic whose deferred calls run,
    /// which is stopped by this; nil where no panic runs them. The
    /// value is a copy of its own.
    pub fn recover(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        if !self.is_iface(ty) {
            return Ok(self.placeholder(span, ty, "`recover` for a place of this type"));
        }
        let zero = Rvalue::Use(self.zero(ty));
        let value = self.slot_of(zero)?;
        let bytes = self.bytes_ty();
        let at = self.record(value.clone())?;
        self.runtime_call("_gopp_recover", &[bytes], vec![at])?;
        self.fresh(expr, ty, Rvalue::Use(Operand::Copy(value)))
    }
}
