//! Elastic tabstops: cells of consecutive lines that end in a tab form a
//! column, and a column is as wide as its widest cell.
//!
//! The input is text with four control bytes: `\t` and `\v` end a cell,
//! `\n` ends a line, and `\f` ends a line and every open column. Text
//! between two [`ESCAPE`] bytes is taken as it is. Leading empty cells are
//! indentation and come out as tabs; all other padding is spaces.

/// Brackets text that may hold control bytes of its own. No UTF-8 text has
/// this byte.
pub(super) const ESCAPE: u8 = 0xFF;

const VTAB: u8 = 0x0B;
const FORMFEED: u8 = 0x0C;
const TAB_WIDTH: usize = 8;
const PADDING: usize = 1;

#[derive(Clone, Copy, Default)]
struct Cell {
    /// Bytes of text.
    size: usize,
    /// Characters of text.
    width: usize,
    /// Ended by `\t`: such a cell keeps its column even when the column is
    /// empty.
    htab: bool,
}

#[derive(Default)]
pub(super) struct Writer {
    out: Vec<u8>,
    /// The text of all buffered cells.
    buf: Vec<u8>,
    /// Up to where in `buf` the width of the open cell is counted.
    counted: usize,
    cell: Cell,
    escaped: bool,
    /// The cells of each buffered line that ended in a tab; the last line
    /// is the open one.
    lines: Vec<Vec<Cell>>,
    widths: Vec<usize>,
    /// Cells handled so far: the measure of work in the scaling tests.
    steps: u64,
}

impl Writer {
    pub fn new() -> Self {
        Self {
            lines: vec![Vec::new()],
            ..Self::default()
        }
    }

    pub fn write(&mut self, text: &[u8]) {
        let mut pending = 0;
        for (i, &byte) in text.iter().enumerate() {
            if self.escaped {
                if byte == ESCAPE {
                    self.append(&text[pending..=i]);
                    pending = i + 1;
                    self.update_width();
                    // The two escape bytes take no room.
                    self.cell.width -= 2;
                    self.escaped = false;
                }
                continue;
            }
            match byte {
                b'\t' | VTAB | b'\n' | FORMFEED => {
                    self.append(&text[pending..i]);
                    self.update_width();
                    pending = i + 1;
                    let cells = self.terminate_cell(byte == b'\t');
                    if byte == b'\n' || byte == FORMFEED {
                        self.lines.push(Vec::new());
                        // A line of one cell has no column to share.
                        if byte == FORMFEED || cells == 1 {
                            self.flush();
                        }
                    }
                }
                ESCAPE => {
                    self.append(&text[pending..i]);
                    self.update_width();
                    pending = i;
                    self.escaped = true;
                }
                _ => {}
            }
        }
        self.append(&text[pending..]);
    }

    /// The laid-out text and the cells it took.
    pub fn finish(mut self) -> (Vec<u8>, u64) {
        self.flush();
        (self.out, self.steps)
    }

    fn append(&mut self, text: &[u8]) {
        self.buf.extend_from_slice(text);
        self.cell.size += text.len();
    }

    fn update_width(&mut self) {
        let text = &self.buf[self.counted..];
        self.cell.width += text.iter().filter(|&&byte| byte & 0xC0 != 0x80).count();
        self.counted = self.buf.len();
    }

    fn terminate_cell(&mut self, htab: bool) -> usize {
        self.cell.htab = htab;
        self.steps += 1;
        let line = self.lines.len() - 1;
        self.lines[line].push(self.cell);
        self.cell = Cell::default();
        self.lines[line].len()
    }

    fn flush(&mut self) {
        if self.cell.size > 0 {
            if self.escaped {
                self.update_width();
                self.cell.width = self.cell.width.saturating_sub(2);
                self.escaped = false;
            }
            self.terminate_cell(false);
        }
        self.format(0, 0, self.lines.len());
        self.buf.clear();
        self.counted = 0;
        self.cell = Cell::default();
        self.lines.clear();
        self.lines.push(Vec::new());
        self.widths.clear();
    }

    /// Lays out the lines `from..to`, whose first `self.widths.len()`
    /// columns are known, and returns the position in `buf` after them.
    fn format(&mut self, mut pos: usize, mut from: usize, to: usize) -> usize {
        let column = self.widths.len();
        let mut this = from;
        while this < to {
            if column + 1 >= self.lines[this].len() {
                this += 1;
                continue;
            }
            // The lines before this one have no cell in the column.
            pos = self.write_lines(pos, from, this);
            from = this;

            let mut width = 0;
            let mut discardable = true;
            while this < to {
                let line = &self.lines[this];
                if column + 1 >= line.len() {
                    break;
                }
                let cell = line[column];
                width = width.max(cell.width + PADDING);
                discardable &= cell.width == 0 && !cell.htab;
                this += 1;
                self.steps += 1;
            }
            if discardable {
                width = 0;
            }

            self.widths.push(width);
            pos = self.format(pos, from, this);
            self.widths.pop();
            from = this;
        }
        self.write_lines(pos, from, to)
    }

    fn write_lines(&mut self, mut pos: usize, from: usize, to: usize) -> usize {
        for index in from..to {
            let mut indenting = true;
            for (column, cell) in self.lines[index].iter().enumerate() {
                let width = self.widths.get(column).copied();
                if cell.size == 0 {
                    if let Some(width) = width {
                        pad(&mut self.out, cell.width, width, indenting);
                    }
                } else {
                    indenting = false;
                    self.out.extend_from_slice(&self.buf[pos..pos + cell.size]);
                    pos += cell.size;
                    if let Some(width) = width {
                        pad(&mut self.out, cell.width, width, false);
                    }
                }
            }
            if index + 1 == self.lines.len() {
                // The open cell of the last line has no end yet.
                self.out
                    .extend_from_slice(&self.buf[pos..pos + self.cell.size]);
                pos += self.cell.size;
            } else {
                self.out.push(b'\n');
            }
        }
        pos
    }
}

fn pad(out: &mut Vec<u8>, text_width: usize, cell_width: usize, tabs: bool) {
    if tabs {
        let cell_width = cell_width.div_ceil(TAB_WIDTH) * TAB_WIDTH;
        let count = cell_width.saturating_sub(text_width).div_ceil(TAB_WIDTH);
        out.extend(std::iter::repeat_n(b'\t', count));
    } else {
        let count = cell_width.saturating_sub(text_width);
        out.extend(std::iter::repeat_n(b' ', count));
    }
}

/// Removes the escape bytes and the blanks at the end of each line, and
/// turns the control bytes that are left into tabs and line ends. Returns
/// the text and whether it has a line end.
pub(super) fn trim(text: &[u8]) -> (Vec<u8>, bool) {
    enum State {
        Space,
        Escape,
        Text,
    }
    let mut out = Vec::with_capacity(text.len());
    let mut space = Vec::new();
    let mut state = State::Space;
    let mut has_newline = false;
    for &byte in text {
        match state {
            State::Escape if byte == ESCAPE => {
                state = State::Space;
                space.clear();
            }
            State::Escape => {
                has_newline |= byte == b'\n' || byte == FORMFEED;
                out.push(byte);
            }
            State::Space | State::Text => match byte {
                b'\t' | VTAB | b' ' => {
                    if matches!(state, State::Text) {
                        space.clear();
                    }
                    state = State::Space;
                    space.push(if byte == b' ' { b' ' } else { b'\t' });
                }
                b'\n' | FORMFEED => {
                    state = State::Space;
                    space.clear();
                    has_newline = true;
                    out.push(b'\n');
                }
                _ => {
                    if matches!(state, State::Space) {
                        out.append(&mut space);
                    }
                    if byte == ESCAPE {
                        state = State::Escape;
                    } else {
                        state = State::Text;
                        out.push(byte);
                    }
                }
            },
        }
    }
    (out, has_newline)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn layout_bytes(text: &[u8]) -> String {
        let mut writer = Writer::new();
        writer.write(text);
        String::from_utf8(trim(&writer.finish().0).0).unwrap()
    }

    fn layout(text: &str) -> String {
        layout_bytes(text.as_bytes())
    }

    #[test]
    fn cells_of_consecutive_lines_share_a_column() {
        assert_eq!(
            layout("a\x0bint\x0b// one\nlonger\x0bstring\x0b// two\n"),
            "a      int    // one\nlonger string // two\n"
        );
    }

    #[test]
    fn a_line_without_the_column_ends_the_block() {
        assert_eq!(layout("a\x0b1\nxx\nlonger\x0b2\n"), "a 1\nxx\nlonger 2\n");
    }

    #[test]
    fn a_formfeed_ends_every_column() {
        assert_eq!(layout("a\x0b1\x0clonger\x0b2\n"), "a 1\nlonger 2\n");
    }

    #[test]
    fn leading_empty_cells_are_tabs() {
        assert_eq!(layout("\t\tx\x0b1\n\t\tyy\x0b2\n"), "\t\tx  1\n\t\tyy 2\n");
    }

    #[test]
    fn an_empty_column_of_soft_cells_is_dropped() {
        assert_eq!(layout("a\x0b\x0bc\nbb\x0b\x0bd\n"), "a  c\nbb d\n");
    }

    #[test]
    fn escaped_text_keeps_its_tabs_and_counts_its_characters() {
        assert_eq!(
            layout_bytes(b"\xffa\tb\xff\x0b1\nc\x0b2\n"),
            "a\tb 1\nc   2\n"
        );
        assert_eq!(
            layout_bytes(b"\xffh\xc3\xa9\xff\x0b1\nabc\x0b2\n"),
            "h\u{e9}  1\nabc 2\n"
        );
    }

    #[test]
    fn nested_columns_are_laid_out_inside_their_block() {
        assert_eq!(
            layout("a\x0bb\x0bc\naa\x0bbb\x0bcc\naaa\x0bbbb\n"),
            "a   b  c\naa  bb cc\naaa bbb\n"
        );
    }

    #[test]
    fn blanks_at_the_end_of_a_line_are_removed() {
        assert_eq!(layout("a\x0b\nb \x0b\n"), "a\nb\n");
        assert_eq!(trim(b"x \t\n").0, b"x\n");
        assert!(!trim(b"x").1);
    }
}
