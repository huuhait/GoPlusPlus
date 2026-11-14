//! What a debugger calls the variables of a function (debug-info.md).
//! A variable of the source has its name in C, and is declared in a
//! block of C that follows its scope, so that two variables of one name
//! are two variables for a debugger too. A name that C would take for
//! something else, and a variable that the unit cannot declare in the
//! block of its scope, stays `_N`: the program means the same.

use std::collections::{HashMap, HashSet};

use gopp_gir::{
    Function, LocalId, LocalKind, Module, Operand, Place, ProjectionKind, Rvalue, ScopeKind,
    Statement, StatementKind, Terminator, TerminatorKind, Type, TypeId,
};

use super::Cx;

/// How many blocks of C may be nested for the scopes of a function:
/// the scopes below share the last.
const MAX_BLOCK_DEPTH: u32 = 64;

/// The words of C, and the names that the unit writes itself: a
/// variable of such a name would be taken for the word.
const TAKEN: [&str; 73] = [
    "NULL",
    "alignas",
    "alignof",
    "and",
    "and_eq",
    "argc",
    "argv",
    "asm",
    "auto",
    "bitand",
    "bitor",
    "bool",
    "break",
    "case",
    "char",
    "compl",
    "const",
    "constexpr",
    "continue",
    "default",
    "do",
    "double",
    "else",
    "enum",
    "extern",
    "false",
    "float",
    "for",
    "goto",
    "if",
    "inline",
    "int",
    "int16_t",
    "int32_t",
    "int64_t",
    "int8_t",
    "intptr_t",
    "long",
    "main",
    "not",
    "not_eq",
    "nullptr",
    "offsetof",
    "or",
    "or_eq",
    "ptrdiff_t",
    "register",
    "restrict",
    "return",
    "short",
    "signed",
    "size_t",
    "sizeof",
    "state",
    "static",
    "static_assert",
    "struct",
    "switch",
    "thread_local",
    "true",
    "typedef",
    "typeof",
    "typeof_unqual",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "uint8_t",
    "uintptr_t",
    "union",
    "unsigned",
    "void",
    "volatile",
    "while",
];

/// Whether a variable of the source may have its name in C.
pub(super) fn usable(name: &str) -> bool {
    let mut letters = name.bytes();
    let starts = letters
        .next()
        .is_some_and(|first| first.is_ascii_alphabetic());
    let numbered = |prefix: &str| {
        (name.strip_prefix(prefix)).is_some_and(|rest| {
            !rest.is_empty() && rest.bytes().all(|digit| digit.is_ascii_digit())
        })
    };
    starts
        && letters.all(|letter| letter.is_ascii_alphanumeric() || letter == b'_')
        && !name.starts_with("gopp")
        && !name.starts_with("GOPP")
        && !numbered("T")
        && !TAKEN.contains(&name)
}

/// How long the tag of a struct may be: a longer one is cut, and ends
/// in a hash of the symbol of the type.
const MAX_TAG: usize = 200;

/// The tag that the struct of each type has in C for a debugger, and
/// the names of its fields: those of the source. A type that keeps the
/// names of the unit has an empty tag, and a field its number.
pub(super) fn of_types(module: &Module, steps: &mut u64) -> (Vec<String>, Vec<Vec<String>>) {
    let types = &module.types;
    let mut tags = vec![String::new(); types.count()];
    let mut fields = vec![Vec::new(); types.count()];
    let mut given: HashSet<String> = HashSet::new();
    for ty in types.ids() {
        *steps += 1;
        let id = match types.get(ty) {
            Type::Struct(id) => id,
            // A function value: its code, which takes the environment
            // first, and the environment.
            Type::Tuple(parts) => {
                let code =
                    |part: &TypeId| matches!(types.get(*part), Type::Func(func) if func.has_env);
                if parts.len() == 2 && parts.first().is_some_and(code) {
                    fields[ty.index()] = vec!["code".to_owned(), "env".to_owned()];
                }
                continue;
            }
            _ => continue,
        };
        // A declared type has the symbol of its descriptor as its
        // name, which says the type as the source writes it.
        let symbol = types.struct_name(*id);
        let readable = gopp_mangle::demangle(symbol.as_bytes(), 2 * MAX_TAG).ok();
        let readable = readable
            .as_deref()
            .and_then(|text| text.strip_prefix("typeinfo for "));
        if let Some(readable) = readable {
            let mut tag = String::with_capacity(readable.len());
            for letter in readable.chars() {
                match letter.is_ascii_alphanumeric() {
                    true => tag.push(letter),
                    false if !tag.ends_with('_') => tag.push('_'),
                    false => {}
                }
            }
            let mut tag = tag.trim_end_matches('_').to_owned();
            if tag.len() > MAX_TAG {
                let hash = xxhash_rust::xxh3::xxh3_64(symbol.as_bytes());
                tag.truncate(MAX_TAG - 20);
                tag = format!("{tag}_x{hash:016x}");
            }
            if usable(&tag) && given.insert(tag.clone()) {
                tags[ty.index()] = tag;
            }
        }
        let mut seen: HashSet<&str> = HashSet::new();
        let numbered = |name: &str| {
            let digits = name.strip_prefix('f').unwrap_or("x");
            digits.bytes().all(|digit| digit.is_ascii_digit())
        };
        let named = (types.field_names(*id).iter().enumerate()).map(|(at, name)| {
            *steps += 1;
            match usable(name) && !numbered(name) && seen.insert(name) {
                true => name.clone(),
                false => format!("f{at}"),
            }
        });
        fields[ty.index()] = named.collect();
    }
    (tags, fields)
}

/// Each place that a statement names.
pub(super) fn places_of(stmt: &Statement, mut each: impl FnMut(&Place)) {
    let StatementKind::Assign(assigned) = &stmt.kind else {
        return;
    };
    let (dest, value) = &**assigned;
    let mut reads = |operand: &Operand| {
        if let Operand::Copy(place) | Operand::Move(place) = operand {
            each(place);
        }
    };
    match value {
        Rvalue::Use(of) | Rvalue::Unary(_, of) | Rvalue::Cast(_, of, _) => reads(of),
        Rvalue::Binary(_, a, b) => [a, b].into_iter().for_each(&mut reads),
        Rvalue::Aggregate(_, parts) => parts.iter().for_each(&mut reads),
        Rvalue::AddrOf(place, _) | Rvalue::Len(place) => each(place),
    }
    each(dest);
}

/// Each place that the end of a block names.
pub(super) fn places_of_end(term: &Terminator, mut each: impl FnMut(&Place)) {
    let mut reads = |operand: &Operand| {
        if let Operand::Copy(place) | Operand::Move(place) = operand {
            each(place);
        }
    };
    match &term.kind {
        TerminatorKind::Switch { on, .. } => reads(on),
        TerminatorKind::Call {
            callee, args, dest, ..
        } => {
            reads(callee);
            args.iter().for_each(&mut reads);
            each(dest);
        }
        TerminatorKind::Assert { cond, args, .. } => {
            reads(cond);
            args.iter().for_each(&mut reads);
        }
        TerminatorKind::Drop { place, .. } => each(place),
        _ => {}
    }
}

/// Each local that a place names: its own, and those that index it.
pub(super) fn locals_of(place: &Place, mut each: impl FnMut(LocalId)) {
    each(place.local);
    for step in &place.projections {
        if let ProjectionKind::Index(at) = step.kind {
            each(at);
        }
    }
}

fn index(local: LocalId) -> usize {
    local.into_raw().into_u32() as usize
}

/// The names of the locals of a function, and the blocks of C that its
/// scopes have.
pub(super) struct Names {
    /// What each local is called in C: its name in the source, or `_N`.
    pub of: Vec<String>,
    /// The scope in whose block each local is declared: none for what
    /// is declared at the top of the function, a parameter, and what
    /// the record of the cleanup blocks holds.
    pub block: Vec<Option<u32>>,
    /// For each scope, from 0 for the function: the scope in whose
    /// block its statements are written.
    pub home: Vec<u32>,
    /// The scopes that have a block of C, each with the scope of the
    /// block around it, the outer before the inner.
    pub blocks: Vec<(u32, u32)>,
}

impl Names {
    /// `_N` for every local, all at the top of the function.
    pub fn plain(func: &Function) -> Self {
        let count = func.locals().len();
        Self {
            of: (0..count).map(|at| format!("_{at}")).collect(),
            block: vec![None; count],
            home: vec![0; func.scopes().len() + 1],
            blocks: Vec::new(),
        }
    }

    /// The names of a function of the source. `shared` says which
    /// locals the record of the cleanup blocks holds: their names are
    /// the names of its fields.
    pub fn of_source(cx: &Cx<'_>, func: &Function, shared: &[bool], steps: &mut u64) -> Self {
        let mut names = Self::plain(func);
        let locals: Vec<_> = func.locals().iter().map(|(_, local)| local).collect();
        let scopes = func.scopes();
        // A scope of the source is numbered from 1; its parent comes
        // before it.
        let parent = |scope: u32| match scope {
            0 => 0,
            scope => scopes[scope as usize - 1].parent.0.min(scope - 1),
        };
        let count = scopes.len() + 1;
        // When the walk over the scopes enters and leaves each: one
        // scope is in another if it is entered and left inside it.
        let mut children: Vec<Vec<u32>> = vec![Vec::new(); count];
        for scope in 1..count as u32 {
            children[parent(scope) as usize].push(scope);
        }
        let (mut entered, mut left) = (vec![0u32; count], vec![0u32; count]);
        let mut clock = 0;
        let mut rest = vec![(0u32, false)];
        while let Some((scope, leaving)) = rest.pop() {
            *steps += 1;
            clock += 1;
            if leaving {
                left[scope as usize] = clock;
                continue;
            }
            entered[scope as usize] = clock;
            rest.push((scope, true));
            rest.extend(
                children[scope as usize]
                    .iter()
                    .rev()
                    .map(|&child| (child, false)),
            );
        }
        let within = |outer: u32, inner: u32| {
            entered[outer as usize] <= entered[inner as usize]
                && left[inner as usize] <= left[outer as usize]
        };

        // The variables that may have their names.
        let named = |at: usize| {
            let local = locals[at];
            let of_source = matches!(local.kind, LocalKind::Var | LocalKind::Arg);
            let name = local.name.as_deref().filter(|name| usable(name))?;
            (of_source && !cx.empty(local.ty)).then_some(name)
        };
        let in_block = |at: usize| {
            let local = locals[at];
            let block = (local.scope.0 as usize)
                .checked_sub(1)
                .is_some_and(|scope| scopes[scope].kind == ScopeKind::Block);
            local.kind == LocalKind::Var && !shared[at] && block
        };
        let mut wanted: Vec<bool> = (0..locals.len()).map(|at| named(at).is_some()).collect();
        // A variable is declared in the block of its scope only if
        // every statement that names it is written in that block.
        let mut outside = |scope: u32, place: &Place| {
            locals_of(place, |local| {
                let at = index(local);
                if wanted[at] && in_block(at) && !within(locals[at].scope.0, scope) {
                    wanted[at] = false;
                }
            });
        };
        for (_, block) in func.blocks().iter().filter(|(_, block)| !block.cleanup) {
            for stmt in &block.stmts {
                *steps += 1;
                places_of(stmt, |place| outside(stmt.scope.0, place));
            }
            places_of_end(&block.term, |place| outside(block.term.scope.0, place));
        }

        // The scopes that have a block of C, at most so many in one
        // another; the scopes below share the last.
        let mut real = vec![false; count];
        for at in (0..locals.len()).filter(|&at| wanted[at] && in_block(at)) {
            real[locals[at].scope.0 as usize] = true;
        }
        let mut depth = vec![0u32; count];
        for scope in 1..count as u32 {
            let above = parent(scope) as usize;
            depth[scope as usize] = depth[above] + u32::from(real[scope as usize]);
            let own = real[scope as usize] && depth[scope as usize] <= MAX_BLOCK_DEPTH;
            names.home[scope as usize] = match own {
                true => scope,
                false => names.home[above],
            };
            if own {
                names.blocks.push((scope, names.home[above]));
            }
        }

        // One variable of a name in a block, in the record, and among
        // the parameters and the variables at the top of the function.
        const RECORD: u32 = u32::MAX;
        let mut holder: HashMap<(u32, &str), usize> = HashMap::new();
        for at in 0..locals.len() {
            *steps += 1;
            let Some(name) = named(at).filter(|_| wanted[at]) else {
                continue;
            };
            let block = match (shared[at], in_block(at)) {
                (true, _) => RECORD,
                (false, true) => names.home[locals[at].scope.0 as usize],
                (false, false) => 0,
            };
            // What is declared at the top of the function is in no
            // scope of the source, so nothing says where it is named.
            let top = block == 0 && locals[at].scope.0 != 0;
            if top || holder.insert((block, name), at).is_some() {
                wanted[at] = false;
            } else if block != RECORD && block != 0 {
                names.block[at] = Some(block);
            }
        }

        // A statement that names a variable from inside a block that
        // declares another of the same name would name that one: the
        // other keeps `_N`.
        let home = &names.home;
        let block = &mut names.block;
        let mut climbed = 0;
        let mut hidden = |scope: u32, place: &Place| {
            locals_of(place, |local| {
                let at = index(local);
                let Some(name) = named(at).filter(|_| wanted[at] && !shared[at]) else {
                    return;
                };
                let declared = block[at].unwrap_or(0);
                let mut inner = home[scope as usize];
                while inner != declared && inner != 0 {
                    climbed += 1;
                    if let Some(&other) = holder.get(&(inner, name)).filter(|&&other| wanted[other])
                    {
                        wanted[other] = false;
                        block[other] = None;
                    }
                    inner = home[parent(inner) as usize];
                }
            });
        };
        for (_, block) in func.blocks().iter().filter(|(_, block)| !block.cleanup) {
            for stmt in &block.stmts {
                *steps += 1;
                places_of(stmt, |place| hidden(stmt.scope.0, place));
            }
            places_of_end(&block.term, |place| hidden(block.term.scope.0, place));
        }
        *steps += climbed;
        for at in (0..locals.len()).filter(|&at| wanted[at]) {
            if let Some(name) = named(at) {
                names.of[at] = name.to_owned();
            }
        }
        names
    }
}
