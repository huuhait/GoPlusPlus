//! The predeclared names.

use lasso::{Spur, ThreadedRodeo};
use rustc_hash::FxHashMap;

macro_rules! predeclared {
    ($($variant:ident $name:literal $kind:ident,)*) => {
        /// A name of the universe block.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        pub enum Predeclared {
            $($variant,)*
        }

        impl Predeclared {
            const ALL: &[Predeclared] = &[$(Predeclared::$variant,)*];

            pub fn name(self) -> &'static str {
                match self {
                    $(Predeclared::$variant => $name,)*
                }
            }

            pub fn kind(self) -> PredeclaredKind {
                match self {
                    $(Predeclared::$variant => PredeclaredKind::$kind,)*
                }
            }
        }
    };
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PredeclaredKind {
    Type,
    Const,
    Nil,
    Func,
}

predeclared! {
    Any "any" Type,
    Bool "bool" Type,
    Byte "byte" Type,
    Comparable "comparable" Type,
    Complex64 "complex64" Type,
    Complex128 "complex128" Type,
    Error "error" Type,
    Float32 "float32" Type,
    Float64 "float64" Type,
    Int "int" Type,
    Int8 "int8" Type,
    Int16 "int16" Type,
    Int32 "int32" Type,
    Int64 "int64" Type,
    Rune "rune" Type,
    String "string" Type,
    Uint "uint" Type,
    Uint8 "uint8" Type,
    Uint16 "uint16" Type,
    Uint32 "uint32" Type,
    Uint64 "uint64" Type,
    Uintptr "uintptr" Type,
    True "true" Const,
    False "false" Const,
    Iota "iota" Const,
    Nil "nil" Nil,
    Append "append" Func,
    Cap "cap" Func,
    Clear "clear" Func,
    Close "close" Func,
    ComplexFunc "complex" Func,
    CopyFunc "copy" Func,
    Delete "delete" Func,
    Imag "imag" Func,
    Len "len" Func,
    Make "make" Func,
    Max "max" Func,
    Min "min" Func,
    Panic "panic" Func,
    Print "print" Func,
    Println "println" Func,
    Real "real" Func,
    Recover "recover" Func,
    Move "move" Func,
    NewUnique "NewUnique" Func,
    NewShared "NewShared" Func,
    NewWeak "NewWeak" Func,
    Unique "Unique" Type,
    Shared "Shared" Type,
    Weak "Weak" Type,
    Option "Option" Type,
    Local "Local" Type,
    Copy "Copy" Type,
    Clone "Clone" Type,
    Send "Send" Type,
    Sync "Sync" Type,
}

/// The universe block.
pub struct Universe {
    scope: FxHashMap<Spur, Predeclared>,
}

impl Universe {
    pub fn new(names: &ThreadedRodeo) -> Self {
        let listed = Predeclared::ALL.iter();
        let scope = listed
            .map(|&name| (names.get_or_intern_static(name.name()), name))
            .collect();
        Self { scope }
    }

    pub fn lookup(&self, name: Spur) -> Option<Predeclared> {
        self.scope.get(&name).copied()
    }
}
