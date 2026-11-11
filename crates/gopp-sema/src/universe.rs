//! The predeclared names.

use gopp_span::Dialect;
use lasso::{Spur, ThreadedRodeo};
use rustc_hash::FxHashMap;

macro_rules! predeclared {
    ($($variant:ident $name:literal $kind:ident $dialect:ident,)*) => {
        /// A name of the universe block.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        pub enum Predeclared {
            $($variant,)*
        }

        impl Predeclared {
            const ALL: &[(Predeclared, In)] = &[$((Predeclared::$variant, In::$dialect),)*];

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

/// The files a predeclared name exists in.
#[derive(Clone, Copy, PartialEq, Eq)]
enum In {
    Both,
    Go,
    Gopp,
}

predeclared! {
    Any "any" Type Both,
    Bool "bool" Type Both,
    Byte "byte" Type Both,
    Comparable "comparable" Type Both,
    Complex64 "complex64" Type Both,
    Complex128 "complex128" Type Both,
    Error "error" Type Both,
    Float32 "float32" Type Both,
    Float64 "float64" Type Both,
    Int "int" Type Both,
    Int8 "int8" Type Both,
    Int16 "int16" Type Both,
    Int32 "int32" Type Both,
    Int64 "int64" Type Both,
    Rune "rune" Type Both,
    String "string" Type Both,
    Uint "uint" Type Both,
    Uint8 "uint8" Type Both,
    Uint16 "uint16" Type Both,
    Uint32 "uint32" Type Both,
    Uint64 "uint64" Type Both,
    Uintptr "uintptr" Type Both,
    True "true" Const Both,
    False "false" Const Both,
    Iota "iota" Const Both,
    Nil "nil" Nil Both,
    Append "append" Func Both,
    Cap "cap" Func Both,
    Clear "clear" Func Both,
    Close "close" Func Both,
    ComplexFunc "complex" Func Both,
    CopyFunc "copy" Func Both,
    Delete "delete" Func Both,
    Imag "imag" Func Both,
    Len "len" Func Both,
    Make "make" Func Both,
    Max "max" Func Both,
    Min "min" Func Both,
    New "new" Func Go,
    Panic "panic" Func Both,
    Print "print" Func Both,
    Println "println" Func Both,
    Real "real" Func Both,
    Recover "recover" Func Both,
    Move "move" Func Gopp,
    NewUnique "NewUnique" Func Gopp,
    NewShared "NewShared" Func Gopp,
    NewWeak "NewWeak" Func Gopp,
    Unique "Unique" Type Gopp,
    Shared "Shared" Type Gopp,
    Weak "Weak" Type Gopp,
    Option "Option" Type Gopp,
    Local "Local" Type Gopp,
    Copy "Copy" Type Gopp,
    Clone "Clone" Type Gopp,
    Send "Send" Type Gopp,
    Sync "Sync" Type Gopp,
}

/// The universe block of each kind of file.
pub struct Universe {
    go: FxHashMap<Spur, Predeclared>,
    gopp: FxHashMap<Spur, Predeclared>,
}

impl Universe {
    pub fn new(names: &ThreadedRodeo) -> Self {
        let scope = |dialect: In| {
            let listed = Predeclared::ALL.iter();
            let here = listed.filter(|(_, within)| *within == In::Both || *within == dialect);
            here.map(|&(name, _)| (names.get_or_intern_static(name.name()), name))
                .collect()
        };
        Self {
            go: scope(In::Go),
            gopp: scope(In::Gopp),
        }
    }

    pub fn lookup(&self, dialect: Dialect, name: Spur) -> Option<Predeclared> {
        let scope = match dialect {
            Dialect::Go => &self.go,
            Dialect::Gopp => &self.gopp,
        };
        scope.get(&name).copied()
    }
}
