//! How large each kind of value is on a target, and how it is aligned.
//! Structs and tuples are laid out as C lays out a struct, in the order
//! of the fields (TYP-12, TYP-13).

/// The size of a value and what its address is a multiple of, in bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    pub size: u64,
    pub align: u64,
}

/// The layout rules of one target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TargetLayout {
    pointer_bits: u8,
}

impl Layout {
    /// A value that takes no room: `struct{}`, no result.
    pub const UNIT: Layout = Layout { size: 0, align: 1 };

    /// `count` values one after the other. `None` if that is more than
    /// a size can say.
    pub fn array(self, count: u64) -> Option<Layout> {
        Some(Layout {
            size: self.size.checked_mul(count)?,
            align: self.align,
        })
    }
}

impl TargetLayout {
    pub fn new(pointer_bits: u8) -> Self {
        Self { pointer_bits }
    }

    /// The width of `int`, `uint` and `uintptr`.
    pub fn pointer_bits(self) -> u8 {
        self.pointer_bits
    }

    /// A value of `bits` bits that the machine holds whole: an integer
    /// or a float. Its address is a multiple of its size.
    pub fn scalar(self, bits: u8) -> Layout {
        let size = u64::from(bits).div_ceil(8);
        Layout {
            size,
            align: size.max(1),
        }
    }

    pub fn bool(self) -> Layout {
        self.scalar(8)
    }

    /// Every kind of pointer: `*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`,
    /// a map, a channel, the code of a function.
    pub fn pointer(self) -> Layout {
        self.scalar(self.pointer_bits)
    }

    /// Values of the layouts `fields` one after the other, each at the
    /// next address that it may have: the whole, and where each field
    /// starts. `None` if that is more than a size can say.
    pub fn aggregate(self, fields: &[Layout]) -> Option<(Layout, Vec<u64>)> {
        let mut whole = Layout::UNIT;
        let mut offsets = Vec::with_capacity(fields.len());
        for field in fields {
            let start = whole.size.checked_next_multiple_of(field.align)?;
            offsets.push(start);
            whole.size = start.checked_add(field.size)?;
            whole.align = whole.align.max(field.align);
        }
        whole.size = whole.size.checked_next_multiple_of(whole.align)?;
        Some((whole, offsets))
    }

    /// `string`: the bytes, how many, and the buffer that holds them.
    pub fn string(self) -> Layout {
        self.words(&[self.pointer(), self.scalar(64), self.pointer()])
    }

    /// `[]T`: the elements, how many, how many fit, and the buffer.
    pub fn slice(self) -> Layout {
        self.words(&[
            self.pointer(),
            self.scalar(64),
            self.scalar(64),
            self.pointer(),
        ])
    }

    /// An interface value: the table of its type, and the value.
    pub fn interface(self) -> Layout {
        self.words(&[self.pointer(), self.pointer()])
    }

    /// A `func` value: the code, and what it captured.
    pub fn func_value(self) -> Layout {
        self.words(&[self.pointer(), self.pointer()])
    }

    /// `Option[T]` for a `T` that has no value to stand for nil: the
    /// value, then whether it is set.
    pub fn option(self, of: Layout) -> Option<Layout> {
        Some(self.aggregate(&[of, self.bool()])?.0)
    }

    /// A few scalars one after the other: never too large.
    fn words(self, fields: &[Layout]) -> Layout {
        let (mut size, mut align) = (0u64, 1u64);
        for field in fields {
            size = size.next_multiple_of(field.align) + field.size;
            align = align.max(field.align);
        }
        Layout {
            size: size.next_multiple_of(align),
            align,
        }
    }
}
