//! A name as Go++ text (appendix A.9): a type as the checker prints it,
//! with package paths in place of package names.

use std::fmt::Write;

use crate::node::{Context, Dir, Node, NodeId, Pair, Required, Wrap};

enum Item<'a> {
    Node(NodeId),
    Text(&'a str),
    Name(&'a [u8]),
    Number(u64),
    Signed(i64),
    /// The types with `, ` between them.
    List(&'a [NodeId]),
    /// The fields or methods with `; ` between them.
    Members(&'a [NodeId]),
    /// The parameters and results of the function type.
    Signature(NodeId),
    /// The slice as the last parameter of a variadic function.
    Variadic(NodeId),
    /// The method with its receiver as a pointer: `(*T).M`.
    Starred(NodeId),
    Tag(&'a [u8]),
}

impl Context {
    /// The node as Go++ text, cut after `at_most` bytes: a type that
    /// occurs often in a name is written out each time.
    pub fn readable(&self, node: NodeId, at_most: usize) -> String {
        self.readable_counted(node, at_most).0
    }

    /// The text, and how many steps writing it took.
    pub(crate) fn readable_counted(&self, node: NodeId, at_most: usize) -> (String, u64) {
        let mut out = String::new();
        let mut rest = vec![Item::Node(node)];
        let mut steps = 0;
        while let Some(item) = rest.pop() {
            if out.len() >= at_most {
                break;
            }
            let (from, before) = (rest.len(), out.len());
            match item {
                Item::Text(text) => out.push_str(text),
                Item::Name(name) => out.push_str(&String::from_utf8_lossy(name)),
                Item::Number(number) => {
                    let _ = write!(out, "{number}");
                }
                Item::Signed(number) => {
                    let _ = write!(out, "{number}");
                }
                Item::Tag(tag) => {
                    let _ = write!(out, " {:?}", String::from_utf8_lossy(tag));
                }
                Item::List(nodes) => separated(nodes, ", ", &mut rest),
                Item::Members(nodes) => separated(nodes, "; ", &mut rest),
                Item::Node(id) => self.node(id, &mut rest),
                Item::Signature(id) => self.signature(id, &mut rest),
                Item::Variadic(id) => {
                    if let Node::Slice(of) = self.get(id) {
                        rest.extend([Item::Text("..."), Item::Node(*of)]);
                    }
                }
                Item::Starred(id) => self.starred(id, &mut rest),
            }
            rest[from..].reverse();
            steps += 1 + (rest.len() - from + out.len() - before) as u64;
        }
        if out.len() > at_most {
            let mut end = at_most;
            while !out.is_char_boundary(end) {
                end -= 1;
            }
            out.truncate(end);
        }
        (out, steps)
    }

    /// What the node is written as, in the order of the text.
    fn node<'a>(&'a self, id: NodeId, rest: &mut Vec<Item<'a>>) {
        use Item::{List, Members, Name, Number, Text};
        let node = Item::Node;
        match self.get(id) {
            Node::Basic(basic) => rest.push(Text(basic.name())),
            Node::Pointer(of) => rest.extend([Text("*"), node(*of)]),
            Node::Slice(of) => rest.extend([Text("[]"), node(*of)]),
            Node::Array(len, of) => rest.extend([Text("["), Number(*len), Text("]"), node(*of)]),
            Node::Map(key, value) => {
                rest.extend([Text("map["), node(*key), Text("]"), node(*value)]);
            }
            Node::Option(of) => rest.extend([Text("Option["), node(*of), Text("]")]),
            Node::Chan(dir, of) => {
                let chan = match dir {
                    Dir::Both => "chan ",
                    Dir::Send => "chan<- ",
                    Dir::Recv => "<-chan ",
                };
                rest.extend([Text(chan), node(*of)]);
            }
            Node::Tuple(types) => rest.extend([Text("("), List(types), Text(")")]),
            Node::Signature { .. } => rest.extend([Text("func"), Item::Signature(id)]),
            Node::Struct(fields) => rest.extend([Text("struct{"), Members(fields), Text("}")]),
            Node::Field {
                name,
                embedded,
                required,
                parent,
                tag,
                ty,
                ..
            } => {
                if !embedded {
                    rest.extend([Name(name), Text(" ")]);
                }
                rest.push(node(*ty));
                rest.extend(match required {
                    Required::No => None,
                    Required::Yes => Some(Text(" @Required")),
                    Required::MustSet => Some(Text(" @MustSet")),
                });
                rest.extend(parent.then_some(Text(" @Parent")));
                rest.extend(tag.as_deref().map(Item::Tag));
            }
            Node::Interface { sync, methods } => match (sync, methods.is_empty()) {
                (false, true) => rest.push(Text("any")),
                (false, false) => rest.extend([Text("interface{"), Members(methods), Text("}")]),
                (true, true) => rest.push(Text("interface{Sync}")),
                (true, false) => {
                    rest.extend([Text("interface{"), Members(methods), Text("; Sync}")]);
                }
            },
            Node::IMethod { name, sig, .. } => rest.extend([Name(name), Item::Signature(*sig)]),
            Node::Named { package, name }
            | Node::Func { package, name }
            | Node::Var { package, name } => rest.extend([Name(package), Text("."), Name(name)]),
            Node::LocalNamed {
                ordinal,
                name,
                within,
            } => {
                rest.extend([node(*within), Text("."), Name(name)]);
                if *ordinal > 0 {
                    rest.extend([Text("."), Number(*ordinal)]);
                }
            }
            Node::Universe(name) => rest.push(Name(name)),
            Node::Instance { of, args } => {
                rest.extend([node(*of), Text("["), List(args), Text("]")]);
            }
            Node::TypeParam(index) => rest.extend([Text("$"), Number(*index)]),
            Node::Method { name, receiver } => {
                rest.extend([node(*receiver), Text("."), Name(name)]);
            }
            Node::OpMethod {
                right,
                name,
                receiver,
                params,
            } => {
                rest.extend([node(*receiver), Text("."), Name(name), node(*params)]);
                rest.extend(right.then_some(Text(" @Right")));
            }
            Node::Closure { ordinal, within } => {
                rest.extend([node(*within), Text(".func"), Number(*ordinal)]);
            }
            Node::PkgInit(package) => rest.extend([Name(package), Text(".init")]),
            Node::InitFunc { package, ordinal } => {
                rest.extend([Name(package), Text(".init."), Number(*ordinal)]);
            }
            Node::Wrap(Wrap::IfacePtrThunk, of) => {
                rest.extend([Text("interface thunk for "), Item::Starred(*of)]);
            }
            Node::Wrap(wrap, of) => {
                let what = match wrap {
                    Wrap::TypeInfo => "typeinfo for ",
                    Wrap::VTable => "vtable for ",
                    Wrap::EqGlue => "eq glue for ",
                    Wrap::HashGlue => "hash glue for ",
                    Wrap::DropGlue => "drop glue for ",
                    Wrap::CloneGlue => "clone glue for ",
                    Wrap::MethodTable => "method table for ",
                    Wrap::IfaceDesc => "interface descriptor for ",
                    Wrap::CabiThunk => "C-ABI thunk for ",
                    Wrap::MethodValue => "method value ",
                    Wrap::MethodExpr => "method expression ",
                    Wrap::Promoted => "promoted method ",
                    Wrap::IfaceThunk | Wrap::IfacePtrThunk => "interface thunk for ",
                };
                rest.extend([Text(what), node(*of)]);
            }
            Node::Pair(pair, first, second) => {
                let what = match pair {
                    Pair::SecondaryVTable => "vtable for ",
                    Pair::ITab => "itab for ",
                };
                rest.extend([Text(what), node(*first), Text(" as "), node(*second)]);
            }
            Node::Data(key) => rest.extend([Text("data "), Name(key)]),
            Node::BaseAdjustThunk(offset, of) => {
                let offset = Item::Signed(*offset);
                rest.extend([Text("thunk at offset "), offset, Text(" for "), node(*of)]);
            }
        }
    }

    /// `(int, ...string) (bool, error)`.
    fn signature<'a>(&'a self, id: NodeId, rest: &mut Vec<Item<'a>>) {
        let Node::Signature {
            variadic,
            params,
            results,
        } = self.get(id)
        else {
            return;
        };
        let (Node::Tuple(params), Node::Tuple(results)) = (self.get(*params), self.get(*results))
        else {
            return;
        };
        rest.push(Item::Text("("));
        match params.split_last() {
            Some((&last, before)) if *variadic => {
                rest.push(Item::List(before));
                rest.extend((!before.is_empty()).then_some(Item::Text(", ")));
                rest.push(Item::Variadic(last));
            }
            _ => rest.push(Item::List(params)),
        }
        rest.push(Item::Text(")"));
        match &results[..] {
            [] => {}
            [one] => rest.extend([Item::Text(" "), Item::Node(*one)]),
            many => rest.extend([Item::Text(" ("), Item::List(many), Item::Text(")")]),
        }
    }

    fn starred<'a>(&'a self, id: NodeId, rest: &mut Vec<Item<'a>>) {
        use Item::{Name, Node as N, Text};
        match self.get(id) {
            Node::Method { name, receiver } => {
                rest.extend([Text("(*"), N(*receiver), Text(")."), Name(name)]);
            }
            Node::OpMethod {
                right,
                name,
                receiver,
                params,
            } => {
                rest.extend([Text("(*"), N(*receiver), Text(")."), Name(name), N(*params)]);
                rest.extend(right.then_some(Text(" @Right")));
            }
            Node::Instance { of, args } => {
                rest.extend([Item::Starred(*of), Text("["), Item::List(args), Text("]")]);
            }
            _ => rest.push(N(id)),
        }
    }
}

fn separated<'a>(nodes: &'a [NodeId], between: &'static str, rest: &mut Vec<Item<'a>>) {
    for (at, &node) in nodes.iter().enumerate() {
        if at > 0 {
            rest.push(Item::Text(between));
        }
        rest.push(Item::Node(node));
    }
}
