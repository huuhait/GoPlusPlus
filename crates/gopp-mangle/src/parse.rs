//! The nodes of a name. Only what the mangler writes is a name: one
//! entity has one text (SYM-5).

use rustc_hash::{FxHashMap, FxHashSet};

use crate::PREFIX;
use crate::ident;
use crate::node::{Basic, Context, Dir, Node, NodeId, Pair, Required, Wrap};

/// Why a text is no name, and where.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ParseError {
    /// The offset in the text, in bytes.
    pub offset: usize,
    pub message: &'static str,
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} at offset {}", self.message, self.offset)
    }
}

impl std::error::Error for ParseError {}

/// A node whose children are being read.
enum Open {
    Pointer,
    Slice,
    Array(u64),
    Map,
    Option,
    Chan(Dir),
    Tuple,
    Signature(bool),
    Struct,
    Field {
        name: Box<[u8]>,
        package: Option<Box<[u8]>>,
        embedded: bool,
        required: Required,
        parent: bool,
        tag: Option<Box<[u8]>>,
    },
    Interface(bool),
    IMethod {
        package: Option<Box<[u8]>>,
        name: Box<[u8]>,
    },
    LocalNamed {
        ordinal: u64,
        name: Box<[u8]>,
    },
    Instance,
    Method(Box<[u8]>),
    OpMethod {
        right: bool,
        name: Box<[u8]>,
    },
    Closure(u64),
    Wrap(Wrap),
    Pair(Pair),
    BaseAdjustThunk(i64),
}

impl Open {
    /// How many children there are; `None` for as many as stand before
    /// an `E`.
    fn arity(&self) -> Option<usize> {
        match self {
            Open::Tuple | Open::Struct | Open::Interface(_) | Open::Instance => None,
            Open::Map | Open::Signature(_) | Open::OpMethod { .. } | Open::Pair(_) => Some(2),
            _ => Some(1),
        }
    }

    /// The node with its children: as many as `arity` says, and at
    /// least the generic thing for an instance.
    fn node(self, children: Vec<NodeId>) -> Node {
        let first = match self {
            Open::Tuple => return Node::Tuple(children.into()),
            Open::Struct => return Node::Struct(children.into()),
            Open::Interface(sync) => {
                let methods = children.into();
                return Node::Interface { sync, methods };
            }
            _ => children[0],
        };
        match self {
            Open::Pointer => Node::Pointer(first),
            Open::Slice => Node::Slice(first),
            Open::Array(len) => Node::Array(len, first),
            Open::Map => Node::Map(first, children[1]),
            Open::Option => Node::Option(first),
            Open::Chan(dir) => Node::Chan(dir, first),
            Open::Signature(variadic) => Node::Signature {
                variadic,
                params: first,
                results: children[1],
            },
            Open::Field {
                name,
                package,
                embedded,
                required,
                parent,
                tag,
            } => Node::Field {
                name,
                package,
                embedded,
                required,
                parent,
                tag,
                ty: first,
            },
            Open::IMethod { package, name } => Node::IMethod {
                package,
                name,
                sig: first,
            },
            Open::LocalNamed { ordinal, name } => Node::LocalNamed {
                ordinal,
                name,
                within: first,
            },
            Open::Instance => Node::Instance {
                of: first,
                args: children[1..].into(),
            },
            Open::Method(name) => Node::Method {
                name,
                receiver: first,
            },
            Open::OpMethod { right, name } => Node::OpMethod {
                right,
                name,
                receiver: first,
                params: children[1],
            },
            Open::Closure(ordinal) => Node::Closure {
                ordinal,
                within: first,
            },
            Open::Wrap(wrap) => Node::Wrap(wrap, first),
            Open::Pair(pair) => Node::Pair(pair, first, children[1]),
            Open::BaseAdjustThunk(offset) => Node::BaseAdjustThunk(offset, first),
            Open::Tuple | Open::Struct | Open::Interface(_) => unreachable!("made above"),
        }
    }
}

struct Frame {
    open: Open,
    start: usize,
    children: Vec<NodeId>,
}

/// What stands at one place of a name.
enum Head {
    /// A node without children, or a back reference to a node.
    Made(NodeId),
    Open(Open),
}

struct Parser<'a> {
    text: &'a [u8],
    at: usize,
    context: &'a mut Context,
    /// The types that back references are written for, by where each
    /// was written out.
    written: FxHashMap<usize, NodeId>,
    seen: FxHashSet<NodeId>,
    steps: u64,
}

/// The entity that the name `text` names, in `context`.
pub fn parse(context: &mut Context, text: &[u8]) -> Result<NodeId, ParseError> {
    parse_counted(context, text).map(|(entity, _)| entity)
}

/// The entity, and how many steps reading the name took.
pub(crate) fn parse_counted(
    context: &mut Context,
    text: &[u8],
) -> Result<(NodeId, u64), ParseError> {
    let mut parser = Parser {
        text,
        at: 0,
        context,
        written: FxHashMap::default(),
        seen: FxHashSet::default(),
        steps: 0,
    };
    let entity = parser.symbol()?;
    Ok((entity, parser.steps))
}

fn error<T>(offset: usize, message: &'static str) -> Result<T, ParseError> {
    Err(ParseError { offset, message })
}

const ENDS_EARLY: &str = "the name ends early";

impl Parser<'_> {
    fn symbol(&mut self) -> Result<NodeId, ParseError> {
        if !self.text.starts_with(PREFIX.as_bytes()) {
            return error(0, "no Go++ symbol: it does not start with _GP");
        }
        self.at = PREFIX.len();
        let mut open: Vec<Frame> = Vec::new();
        loop {
            self.steps += 1;
            let start = self.at;
            let ends = self.peek() == Some(b'E');
            let list = open.pop_if(|frame| ends && frame.open.arity().is_none());
            let mut made = match list {
                Some(frame) => {
                    self.at += 1;
                    self.close(frame)?
                }
                None => match self.head()? {
                    Head::Made(id) => id,
                    Head::Open(kind) => {
                        open.push(Frame {
                            open: kind,
                            start,
                            children: Vec::new(),
                        });
                        continue;
                    }
                },
            };
            // The node is the last child of as many as end with it.
            loop {
                let Some(frame) = open.last_mut() else {
                    return match (self.at == self.text.len(), self.context.is_entity(made)) {
                        (false, _) => error(self.at, "there is text after the name"),
                        (true, false) => error(PREFIX.len(), "what is named is no entity"),
                        (true, true) => Ok(made),
                    };
                };
                frame.children.push(made);
                if frame.open.arity() != Some(frame.children.len()) {
                    break;
                }
                match open.pop() {
                    Some(frame) => made = self.close(frame)?,
                    None => break,
                }
            }
        }
    }

    /// The node of a frame that has all its children.
    fn close(&mut self, frame: Frame) -> Result<NodeId, ParseError> {
        if frame.children.is_empty() && matches!(frame.open, Open::Instance) {
            return error(frame.start, "an instance of nothing");
        }
        self.steps += 1;
        self.make(frame.open.node(frame.children), frame.start)
    }

    /// The node that was read from `start` on.
    fn make(&mut self, node: Node, start: usize) -> Result<NodeId, ParseError> {
        let id = match self.context.add(node) {
            Ok(id) => id,
            Err(bad) => return error(start, bad.0),
        };
        if self.context.is_referred(id) {
            if !self.seen.insert(id) {
                return error(
                    start,
                    "a type is written out again where a back reference is written",
                );
            }
            self.written.insert(start, id);
        }
        Ok(id)
    }

    fn peek(&self) -> Option<u8> {
        self.text.get(self.at).copied()
    }

    fn byte(&mut self) -> Result<u8, ParseError> {
        let Some(byte) = self.peek() else {
            return error(self.at, ENDS_EARLY);
        };
        self.at += 1;
        Ok(byte)
    }

    fn expect(&mut self, text: &[u8], message: &'static str) -> Result<(), ParseError> {
        if !self.text[self.at..].starts_with(text) {
            return match self.at + text.len() > self.text.len() {
                true => error(self.text.len(), ENDS_EARLY),
                false => error(self.at, message),
            };
        }
        self.at += text.len();
        Ok(())
    }

    /// Decimal digits without a leading zero.
    fn digits(&mut self, message: &'static str) -> Result<u64, ParseError> {
        let start = self.at;
        let mut value: u64 = 0;
        while let Some(digit @ b'0'..=b'9') = self.peek() {
            let more = value
                .checked_mul(10)
                .and_then(|value| value.checked_add(u64::from(digit - b'0')));
            let leading_zero = self.at > start && value == 0;
            match more {
                Some(more) if !leading_zero => value = more,
                Some(_) => return error(start, "a number has a leading zero"),
                None => return error(start, "a number is too large"),
            }
            self.at += 1;
        }
        match (self.at == start, self.at == self.text.len()) {
            (true, true) => error(self.at, ENDS_EARLY),
            (true, false) => error(start, message),
            _ => Ok(value),
        }
    }

    fn number(&mut self) -> Result<u64, ParseError> {
        let value = self.digits("a number is expected")?;
        self.expect(b"_", "a number does not end with _")?;
        Ok(value)
    }

    fn signed(&mut self) -> Result<i64, ParseError> {
        let start = self.at;
        let negative = self.peek() == Some(b'n');
        self.at += usize::from(negative);
        let value = self.number()?;
        let signed = match negative {
            true if value == 0 => None,
            true => 0i64.checked_sub_unsigned(value),
            false => i64::try_from(value).ok(),
        };
        match signed {
            Some(signed) => Ok(signed),
            None => error(start, "a signed number is out of range, or is -0"),
        }
    }

    fn identifier(&mut self) -> Result<Box<[u8]>, ParseError> {
        let start = self.at;
        let len = self.digits("an identifier is expected")?;
        if len == 0 {
            self.expect(b"_", "the empty identifier is not written 0_")?;
            return Ok(Box::default());
        }
        let end = usize::try_from(len)
            .ok()
            .and_then(|len| self.at.checked_add(len))
            .filter(|&end| end <= self.text.len());
        let Some(end) = end else {
            return error(start, "an identifier is longer than the name");
        };
        let escaped = &self.text[self.at..end];
        self.steps += escaped.len() as u64;
        let bad = "a character is not escaped as the mangler escapes it";
        let Some(bytes) = ident::unescape(escaped) else {
            return error(self.at, bad);
        };
        let mut again = String::with_capacity(escaped.len());
        ident::escape(&bytes, &mut again);
        if again.as_bytes() != escaped {
            return error(self.at, bad);
        }
        self.at = end;
        Ok(bytes.into())
    }

    fn path(&mut self) -> Result<Box<[u8]>, ParseError> {
        let mut path = Vec::new();
        loop {
            let start = self.at;
            let segment = self.identifier()?;
            if segment.contains(&b'/') {
                return error(start, "a segment of a package path holds a /");
            }
            path.extend_from_slice(&segment);
            if self.peek() == Some(b'E') {
                self.at += 1;
                return Ok(path.into());
            }
            path.push(b'/');
        }
    }

    fn leaf(&mut self, node: Node, start: usize) -> Result<Head, ParseError> {
        self.make(node, start).map(Head::Made)
    }

    fn head(&mut self) -> Result<Head, ParseError> {
        let start = self.at;
        let tag = self.byte()?;
        if let Some(&basic) = Basic::ALL.iter().find(|basic| basic.tag() == tag as char) {
            return self.leaf(Node::Basic(basic), start);
        }
        let open = match tag {
            b'P' => Open::Pointer,
            b'S' => Open::Slice,
            b'A' => Open::Array(self.number()?),
            b'H' => Open::Map,
            b'O' => Open::Option,
            b'X' => Open::Chan(match self.byte()? {
                b'b' => Dir::Both,
                b's' => Dir::Send,
                b'r' => Dir::Recv,
                _ => return error(start + 1, "a channel is b, s or r"),
            }),
            b'L' => Open::Tuple,
            b'G' => {
                let variadic = match self.number()? {
                    0 => false,
                    1 => true,
                    _ => return error(start + 1, "a function type has unknown flags"),
                };
                self.expect(b"0_", "the second number of a function type is not 0")?;
                Open::Signature(variadic)
            }
            b'Z' => {
                self.expect(b"0_", "the number of a struct is not 0")?;
                Open::Struct
            }
            b'q' => self.field(start)?,
            b'Y' => match self.peek() {
                Some(b'0'..=b'9') => match self.number()? {
                    1 => Open::Interface(true),
                    _ => return error(start + 1, "an interface has unknown flags"),
                },
                _ => Open::Interface(false),
            },
            b'n' => {
                let package = match self.byte()? {
                    b'0' => None,
                    b'1' => Some(self.path()?),
                    _ => return error(start + 1, "a method of an interface is 0 or 1"),
                };
                let name = self.identifier()?;
                Open::IMethod { package, name }
            }
            b'N' => {
                let (package, name) = (self.path()?, self.identifier()?);
                return self.leaf(Node::Named { package, name }, start);
            }
            b'g' => {
                let (ordinal, name) = (self.number()?, self.identifier()?);
                Open::LocalNamed { ordinal, name }
            }
            b'u' => {
                let name = self.identifier()?;
                return self.leaf(Node::Universe(name), start);
            }
            b'I' => Open::Instance,
            b'Q' => {
                let index = self.number()?;
                return self.leaf(Node::TypeParam(index), start);
            }
            b'B' => {
                let offset = self.number()?;
                let found = usize::try_from(offset)
                    .ok()
                    .and_then(|offset| self.written.get(&offset));
                return match found {
                    Some(&id) => Ok(Head::Made(id)),
                    None => error(start, "a back reference to no type that is written out"),
                };
            }
            b'F' => {
                let (package, name) = (self.path()?, self.identifier()?);
                return self.leaf(Node::Func { package, name }, start);
            }
            b'M' => Open::Method(self.identifier()?),
            b'R' => {
                let (package, name) = (self.path()?, self.identifier()?);
                return self.leaf(Node::Var { package, name }, start);
            }
            b'C' => Open::Closure(self.number()?),
            b'J' => {
                let package = self.path()?;
                return self.leaf(Node::PkgInit(package), start);
            }
            b'K' => {
                let (package, ordinal) = (self.path()?, self.number()?);
                return self.leaf(Node::InitFunc { package, ordinal }, start);
            }
            b'T' => return self.long_tag(start),
            _ => return error(start, "an unknown tag"),
        };
        Ok(Head::Open(open))
    }

    /// The node of a tag that starts with `T`.
    fn long_tag(&mut self, start: usize) -> Result<Head, ParseError> {
        let tag = [b'T', self.byte()?];
        if let Some(&wrap) = Wrap::ALL.iter().find(|wrap| wrap.tag().as_bytes() == tag) {
            return Ok(Head::Open(Open::Wrap(wrap)));
        }
        let open = match tag[1] {
            b'o' => {
                let at = self.at;
                let name = self.identifier()?;
                let (right, name) = match (name.strip_prefix(b"opr_"), name.strip_prefix(b"op_")) {
                    (Some(name), _) => (true, name.into()),
                    (None, Some(name)) => (false, name.into()),
                    (None, None) => {
                        return error(at, "an operator method is named op_ or opr_ and a name");
                    }
                };
                Open::OpMethod { right, name }
            }
            b's' => Open::Pair(Pair::SecondaryVTable),
            b'a' => Open::Pair(Pair::ITab),
            b'D' => {
                let key = self.identifier()?;
                return self.leaf(Node::Data(key), start);
            }
            b't' => Open::BaseAdjustThunk(self.signed()?),
            _ => return error(start, "an unknown tag"),
        };
        Ok(Head::Open(open))
    }

    fn field(&mut self, start: usize) -> Result<Open, ParseError> {
        let flags = self.number()?;
        let has = |bit: u64| flags & bit != 0;
        if flags & !(1 | 2 | 4 | 8 | 32 | 128) != 0 || (has(8) && has(128)) {
            return error(start + 1, "a field has unknown flags");
        }
        let package = match has(2) {
            true => Some(self.path()?),
            false => None,
        };
        let name = self.identifier()?;
        let tag = match has(4) {
            true => Some(self.identifier()?),
            false => None,
        };
        let required = match (has(8), has(128)) {
            (true, _) => Required::Yes,
            (false, true) => Required::MustSet,
            (false, false) => Required::No,
        };
        Ok(Open::Field {
            name,
            package,
            embedded: has(1),
            required,
            parent: has(32),
            tag,
        })
    }
}
