//! The text of documentation comments in canonical form: paragraphs, headings (`# Title`), code blocks (indented by
//! a tab), lists (` - item`, `1. item`) and link definitions
//! (`[text]: url`, moved to the end), with one blank line between them.

/// One block of a documentation comment.
enum Block {
    Paragraph(String),
    Heading(String),
    Code(String),
    List(List),
}

struct List {
    /// A blank line is before the list in the source.
    blank_before: bool,
    /// A blank line is between two of its items.
    blank_between: bool,
    items: Vec<Item>,
}

struct Item {
    /// Empty for a bullet.
    number: String,
    paragraphs: Vec<String>,
}

struct LinkDef {
    text: String,
    url: String,
    used: bool,
}

impl List {
    fn is_loose(&self) -> bool {
        self.blank_between || self.items.iter().any(|item| item.paragraphs.len() != 1)
    }
}

fn is_indented(line: &str) -> bool {
    line.starts_with([' ', '\t'])
}

fn leading_space(line: &str) -> &str {
    let end = line.find(|c| c != ' ' && c != '\t').unwrap_or(line.len());
    &line[..end]
}

/// The lines without the blank ones at both ends and without the white
/// space that all of them start with.
fn unindent<'t>(mut lines: &[&'t str]) -> Vec<&'t str> {
    while let [first, rest @ ..] = lines
        && first.is_empty()
    {
        lines = rest;
    }
    while let [rest @ .., last] = lines
        && last.is_empty()
    {
        lines = rest;
    }
    let Some(first) = lines.first() else {
        return Vec::new();
    };
    let mut prefix = leading_space(first);
    for line in &lines[1..] {
        if !line.is_empty() {
            let space = leading_space(line);
            let shared = prefix
                .bytes()
                .zip(space.bytes())
                .take_while(|(a, b)| a == b);
            prefix = &prefix[..shared.count()];
        }
    }
    let strip = |line: &&'t str| {
        let line = line.strip_prefix(prefix).unwrap_or(line);
        if line.trim().is_empty() { "" } else { line }
    };
    let mut out: Vec<&str> = lines.iter().map(strip).collect();
    let content = out
        .iter()
        .position(|line| !line.is_empty())
        .unwrap_or(out.len());
    out.drain(..content);
    while out.last().is_some_and(|line| line.is_empty()) {
        out.pop();
    }
    out
}

/// The number of a list item (empty for a bullet) and the text after its
/// marker.
fn list_marker(line: &str) -> Option<(&str, &str)> {
    let line = line.trim();
    let first = line.chars().next()?;
    let (number, rest) = if matches!(first, '\u{2022}' | '*' | '+' | '-') {
        ("", &line[first.len_utf8()..])
    } else if first.is_ascii_digit() {
        let digits = line.find(|c: char| !c.is_ascii_digit())?;
        if !line[digits..].starts_with(['.', ')']) {
            return None;
        }
        (&line[..digits], &line[digits + 1..])
    } else {
        return None;
    };
    (is_indented(rest) && !rest.trim().is_empty()).then_some((number, rest))
}

fn is_list(line: &str) -> bool {
    list_marker(line).is_some()
}

fn is_heading(line: &str) -> bool {
    (line.starts_with("# ") || line.starts_with("#\t")) && line.trim() != "#"
}

/// A heading of the old form: a line of its own between blank lines that
/// starts with a capital letter and has no punctuation.
fn is_old_heading(line: &str, all: &[&str], at: usize) -> bool {
    let alone = at > 0
        && all[at - 1].is_empty()
        && at + 2 < all.len()
        && all[at + 1].is_empty()
        && leading_space(all[at + 2]).is_empty();
    let line = line.trim();
    let (Some(first), Some(last)) = (line.chars().next(), line.chars().next_back()) else {
        return false;
    };
    if !alone
        || !first.is_alphabetic()
        || !first.is_uppercase()
        || !last.is_alphanumeric()
        || line.contains(|c| ";:!?+*/=[]{}_^\u{b0}&\u{a7}~%#@<\">\\".contains(c))
    {
        return false;
    }
    // An apostrophe is only for a possessive, a period only inside a word.
    let after = |mark: char| line.match_indices(mark).map(move |(at, _)| &line[at + 1..]);
    after('\'').all(|rest| rest == "s" || rest.starts_with("s "))
        && after('.').all(|rest| !rest.is_empty() && !rest.starts_with(' '))
}

const SCHEMES: [&str; 7] = ["file", "ftp", "gopher", "http", "https", "mailto", "nntp"];

fn is_host(c: u8) -> bool {
    c.is_ascii_alphanumeric() || b"_@-.[]:".contains(&c)
}

fn is_punct(c: u8) -> bool {
    b".,:;?!".contains(&c)
}

fn is_path(c: u8) -> bool {
    c.is_ascii_alphanumeric() || b"$'()*+&#=@~_/-[]{}%".contains(&c)
}

/// The length of the URL that `text` starts with, if it starts with one.
fn auto_url(text: &str) -> Option<usize> {
    let bytes = text.as_bytes();
    let colon = (3..=6).find(|&at| bytes.get(at) == Some(&b':'))?;
    if bytes.len() < 7 || !text[colon..].starts_with("://") || !SCHEMES.contains(&&text[..colon]) {
        return None;
    }
    let mut at = colon + 3;
    if !bytes.get(at).is_some_and(|&c| is_host(c) && !is_punct(c)) {
        return None;
    }
    at += 1;
    // Punctuation at the end is not part of the URL.
    let mut end = at;
    while bytes.get(at).is_some_and(|&c| is_host(c)) {
        if !is_punct(bytes[at]) {
            end = at + 1;
        }
        at += 1;
    }
    at = end;
    let mut open = Vec::new();
    while at < bytes.len() {
        let c = bytes[at];
        if !is_punct(c) {
            if !is_path(c) {
                break;
            }
            if matches!(c, b')' | b'}' | b']') && open.pop() != Some(c) {
                break;
            }
            match c {
                b'(' => open.push(b')'),
                b'{' => open.push(b'}'),
                b'[' => open.push(b']'),
                _ => {}
            }
            if open.is_empty() {
                end = at + 1;
            }
        }
        at += 1;
    }
    Some(end)
}

fn ident_length(text: &str) -> usize {
    let is_ident =
        |(at, c): (usize, char)| c == '_' || c.is_alphabetic() || c.is_ascii_digit() && at > 0;
    let end = text.char_indices().find(|&pair| !is_ident(pair));
    end.map_or(text.len(), |(at, _)| at)
}

/// Text with the quotes of the old form, two backquotes and two
/// apostrophes, as real quotation marks. URLs stay as they are.
fn with_quotes(text: &str, out: &mut String) {
    let mut at = 0;
    while at < text.len() {
        let rest = &text[at..];
        if let Some(url) = auto_url(rest) {
            out.push_str(&rest[..url]);
            at += url;
            continue;
        }
        let ident = ident_length(rest);
        if ident > 0 {
            out.push_str(&rest[..ident]);
            at += ident;
            continue;
        }
        if rest.starts_with("```") {
            // The run of backquotes ends where the first three end; what
            // follows them on the line is skipped.
            let mut end = at + 3;
            while rest.as_bytes().get(end) == Some(&b'`') {
                end += 1;
            }
            let end = end.min(text.len());
            out.push_str(&text[at..end]);
            at = end;
        } else if rest.starts_with("``") {
            out.push('\u{201C}');
            at += 2;
        } else if rest.starts_with("''") {
            out.push('\u{201D}');
            at += 2;
        } else {
            let c = rest.chars().next().map_or(1, char::len_utf8);
            out.push_str(&rest[..c]);
            at += c;
        }
    }
}

struct Parser {
    blocks: Vec<Block>,
    links: Vec<LinkDef>,
}

impl Parser {
    /// A paragraph, or nothing if every line is a link definition.
    fn paragraph(&mut self, lines: &[&str]) -> Option<String> {
        let link = |line: &&str| {
            let rest = line.strip_prefix('[')?;
            let (text, url) = rest.split_once("]:")?;
            if !is_indented(url) || url.len() < 2 {
                return None;
            }
            let url = url[1..].trim();
            let scheme = url.find("://")?;
            SCHEMES.contains(&&url[..scheme]).then(|| LinkDef {
                text: text.to_owned(),
                url: url.to_owned(),
                used: false,
            })
        };
        match lines.iter().map(link).collect::<Option<Vec<LinkDef>>>() {
            Some(links) => {
                self.links.extend(links);
                None
            }
            None => Some(lines.join("\n")),
        }
    }

    fn list(&mut self, lines: &[&str], blank_before: bool) -> List {
        let numbered = list_marker(lines[0]).is_some_and(|(number, _)| !number.is_empty());
        let mut list = List {
            blank_before,
            blank_between: false,
            items: Vec::new(),
        };
        let mut text: Vec<&str> = Vec::new();
        for &line in lines {
            let mut line = line;
            if let Some((number, rest)) = list_marker(line)
                && number.is_empty() != numbered
            {
                self.end_item(&mut list, &mut text);
                list.items.push(Item {
                    number: number.to_owned(),
                    paragraphs: Vec::new(),
                });
                line = rest;
            }
            let line = line.trim();
            if line.is_empty() {
                list.blank_between = true;
                self.end_item(&mut list, &mut text);
            } else {
                text.push(line);
            }
        }
        self.end_item(&mut list, &mut text);
        list
    }

    fn end_item(&mut self, list: &mut List, text: &mut Vec<&str>) {
        if let Some(item) = list.items.last_mut() {
            item.paragraphs.extend(self.paragraph(text));
        }
        text.clear();
    }

    fn parse(&mut self, lines: &[&str]) {
        let count = lines.len();
        let indented = |at: usize| is_indented(lines[at]);
        let mut at = 0;
        // Lines before this one are taken as indented: the lines after a
        // list item or an opening brace that the writer did not indent.
        let mut force_indent = 0;
        let mut prev_end = 0;
        loop {
            while at < count && lines[at].is_empty() {
                at += 1;
            }
            if at >= count {
                break;
            }
            let start = at;
            let mut end;
            if at < force_indent || indented(at) {
                let mut bare_list_ok = is_list(lines[at]) && at < force_indent;
                at += 1;
                while at < count
                    && (lines[at].is_empty()
                        || at < force_indent
                        || indented(at)
                        || bare_list_ok && is_list(lines[at]))
                {
                    bare_list_ok &= !lines[at].is_empty();
                    at += 1;
                }
                end = at;
                while end > start && lines[end - 1].is_empty() {
                    end -= 1;
                }
                // The closing brace of a code block that is not indented.
                if end < count && lines[end].starts_with('}') {
                    end += 1;
                }
                let block = if is_list(lines[start]) {
                    Block::List(self.list(&lines[start..end], prev_end < start))
                } else {
                    let mut code = unindent(&lines[start..end]).join("\n");
                    code.push('\n');
                    Block::Code(code)
                };
                self.blocks.push(block);
            } else {
                at += 1;
                while at < count && !lines[at].is_empty() && !indented(at) {
                    at += 1;
                }
                end = at;
                if at < count && !lines[at].is_empty() && !is_list(lines[at]) {
                    if is_list(lines[at - 1]) {
                        force_indent = end;
                        end -= 1;
                        while end > start && is_list(lines[end - 1]) {
                            end -= 1;
                        }
                    } else if lines[at - 1].ends_with(['{', '\\']) {
                        force_indent = end;
                        end -= 1;
                    }
                    if start == end && force_indent > start {
                        at = start;
                        continue;
                    }
                }
                let block = if end - start == 1 && is_heading(lines[start]) {
                    Some(Block::Heading(lines[start][1..].trim().to_owned()))
                } else if end - start == 1 && is_old_heading(lines[start], lines, start) {
                    Some(Block::Heading(lines[start].trim().to_owned()))
                } else {
                    self.paragraph(&lines[start..end]).map(Block::Paragraph)
                };
                self.blocks.extend(block);
            }
            prev_end = end;
            at = end;
        }
    }

    /// Marks the link definitions that a paragraph uses as `[text]`.
    fn mark_used(&mut self, paragraph: &str) {
        let mut open = false;
        let mut inside = String::new();
        for c in paragraph.chars() {
            let c = if c == '\n' || c == '\t' { ' ' } else { c };
            match c {
                '[' => {
                    open = true;
                    continue;
                }
                ']' => {
                    if open
                        && let Some(link) = self.links.iter_mut().find(|link| link.text == inside)
                    {
                        link.used = true;
                    }
                    open = false;
                    inside.clear();
                }
                _ => {}
            }
            if open {
                inside.push(c);
            }
        }
    }
}

/// The text of a documentation comment, without its comment markers, in
/// canonical form.
pub(super) fn reformat(text: &str) -> String {
    let lines: Vec<&str> = text.split('\n').collect();
    let lines = unindent(&lines);
    let mut parser = Parser {
        blocks: Vec::new(),
        links: Vec::new(),
    };
    parser.parse(&lines);
    let blocks = std::mem::take(&mut parser.blocks);
    for block in &blocks {
        match block {
            Block::Paragraph(text) => parser.mark_used(text),
            Block::List(list) => {
                let paragraphs = list.items.iter().flat_map(|item| &item.paragraphs);
                paragraphs.for_each(|text| parser.mark_used(text));
            }
            Block::Heading(_) | Block::Code(_) => {}
        }
    }

    let mut out = String::new();
    for (index, block) in blocks.iter().enumerate() {
        let tight_list =
            matches!(block, Block::List(list) if !list.blank_before && !list.is_loose());
        if index > 0 && !tight_list {
            out.push('\n');
        }
        match block {
            Block::Paragraph(text) => {
                with_quotes(text, &mut out);
                out.push('\n');
            }
            Block::Heading(text) => {
                out.push_str("# ");
                out.push_str(text);
                out.push('\n');
            }
            Block::Code(text) => {
                for line in text.lines() {
                    if !line.is_empty() {
                        out.push('\t');
                        out.push_str(line);
                    }
                    out.push('\n');
                }
            }
            Block::List(list) => {
                for (index, item) in list.items.iter().enumerate() {
                    if index > 0 && list.is_loose() {
                        out.push('\n');
                    }
                    if item.number.is_empty() {
                        out.push_str("  - ");
                    } else {
                        out.push(' ');
                        out.push_str(&item.number);
                        out.push_str(". ");
                    }
                    for (index, paragraph) in item.paragraphs.iter().enumerate() {
                        if index > 0 {
                            out.push_str("\n    ");
                        }
                        let mut text = String::new();
                        with_quotes(paragraph, &mut text);
                        out.push_str(&text.replace('\n', "\n    "));
                        out.push('\n');
                    }
                }
            }
        }
    }
    for used in [true, false] {
        let mut links = parser
            .links
            .iter()
            .filter(|link| link.used == used)
            .peekable();
        if links.peek().is_some() {
            out.push('\n');
        }
        for link in links {
            out.push_str(&format!("[{}]: {}\n", link.text, link.url));
        }
    }
    out
}

/// Whether a line comment, without its `//`, is a directive such as
/// `lint:ignore` and no text.
pub(super) fn is_directive(comment: &str) -> bool {
    if ["line ", "extern ", "export "]
        .iter()
        .any(|prefix| comment.starts_with(prefix))
    {
        return true;
    }
    let word = |c: u8| c.is_ascii_lowercase() || c.is_ascii_digit();
    match comment.split_once(':') {
        Some((name, rest)) => {
            !name.is_empty() && name.bytes().all(word) && rest.bytes().next().is_some_and(word)
        }
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paragraphs_code_and_lists_get_their_canonical_form() {
        assert_eq!(
            reformat("Text here.\n   code\n     more\nText again.\n"),
            "Text here.\n\n\tcode\n\t  more\n\nText again.\n"
        );
        assert_eq!(
            reformat("List:\n - one\n - two\n   more\nEnd.\n"),
            "List:\n  - one\n  - two\n    more\n\nEnd.\n"
        );
        assert_eq!(
            reformat("Steps:\n 1. one\n 2) two\n\nNot a list:\n\n1. one\n"),
            "Steps:\n 1. one\n 2. two\n\nNot a list:\n\n1. one\n"
        );
        assert_eq!(reformat("\n\nOnly this.\n\n\n"), "Only this.\n");
    }

    #[test]
    fn headings_links_and_quotes() {
        assert_eq!(
            reformat(
                "Intro.\n\nOld Heading\n\nText with ``quotes'' and [a link].\n\n[a link]: https://example.org/''x\n[unused]: http://example.com\n"
            ),
            "Intro.\n\n# Old Heading\n\nText with \u{201C}quotes\u{201D} and [a link].\n\n[a link]: https://example.org/''x\n\n[unused]: http://example.com\n"
        );
        assert_eq!(
            reformat("See http://x.y/a''b, ok.\n"),
            "See http://x.y/a''b, ok.\n"
        );
    }

    #[test]
    fn directives_are_told_from_text() {
        assert!(is_directive("lint:ignore"));
        assert!(is_directive("line foo.gopp:10"));
        assert!(!is_directive(" lint:ignore"));
        assert!(!is_directive("Note: text"));
        assert!(!is_directive("lint: text"));
    }
}
