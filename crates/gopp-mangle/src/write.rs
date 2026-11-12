//! A name from its nodes (appendix A.4, A.6).

use std::fmt::Write;

use rustc_hash::FxHashMap;

use crate::PREFIX;
use crate::node::{Context, NodeId};

enum Item {
    Node(NodeId),
    End,
}

impl Context {
    /// The symbol of the entity.
    pub fn symbol(&self, entity: NodeId) -> String {
        self.symbol_counted(entity).0
    }

    /// The symbol, and how many steps writing it took.
    pub(crate) fn symbol_counted(&self, entity: NodeId) -> (String, u64) {
        debug_assert!(self.is_entity(entity));
        let mut out = String::from(PREFIX);
        // Where each type that is long enough was written first.
        let mut first: FxHashMap<NodeId, usize> = FxHashMap::default();
        let mut rest = vec![Item::Node(entity)];
        let mut steps = 0;
        while let Some(item) = rest.pop() {
            steps += 1;
            let id = match item {
                Item::Node(id) => id,
                Item::End => {
                    out.push('E');
                    continue;
                }
            };
            if self.is_referred(id) {
                if let Some(at) = first.get(&id) {
                    let _ = write!(out, "B{at}_");
                    continue;
                }
                first.insert(id, out.len());
            }
            let node = self.get(id);
            let before = out.len();
            node.head(&mut out);
            steps += (out.len() - before) as u64;
            if node.closes() {
                rest.push(Item::End);
            }
            let from = rest.len();
            node.each_child(&mut |child| rest.push(Item::Node(child)));
            rest[from..].reverse();
        }
        (out, steps)
    }
}
