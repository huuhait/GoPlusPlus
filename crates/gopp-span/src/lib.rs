//! Source files, spans, and line/column lookup.

use std::path::{Path, PathBuf};
use std::sync::{Arc, PoisonError, RwLock};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FileId(u32);

/// A byte range `lo..hi` of one file.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Span {
    pub file: FileId,
    pub lo: u32,
    pub hi: u32,
}

/// A 1-based line and a 1-based column counted in bytes, as Go reports them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LineCol {
    pub line: u32,
    pub column: u32,
}

/// Whether the path names a Go++ source file: one that ends in `.gopp`.
pub fn is_source_path(path: &Path) -> bool {
    path.extension()
        .is_some_and(|extension| extension == "gopp")
}

#[derive(Debug, thiserror::Error)]
#[error("{}: file is larger than 4 GiB", path.display())]
pub struct FileTooLarge {
    pub path: PathBuf,
}

#[derive(Debug)]
pub struct SourceFile {
    path: PathBuf,
    text: String,
    line_starts: Vec<u32>,
    invalid_utf8: Vec<u32>,
    utf16: bool,
}

impl SourceFile {
    pub fn new(path: PathBuf, text: String) -> Result<Self, FileTooLarge> {
        Self::decoded(path, text, Vec::new(), false)
    }

    /// Reads a file's bytes as UTF-8. Each invalid sequence becomes one
    /// U+FFFD in [`text`](Self::text), listed in
    /// [`invalid_utf8`](Self::invalid_utf8).
    pub fn from_bytes(path: PathBuf, bytes: &[u8]) -> Result<Self, FileTooLarge> {
        let utf16 = bytes.starts_with(&[0xFF, 0xFE]) || bytes.starts_with(&[0xFE, 0xFF]);
        let mut text = String::with_capacity(bytes.len());
        let mut invalid_utf8 = Vec::new();
        for chunk in bytes.utf8_chunks() {
            text.push_str(chunk.valid());
            if !chunk.invalid().is_empty() {
                if u32::try_from(text.len()).is_err() {
                    return Err(FileTooLarge { path });
                }
                invalid_utf8.push(text.len() as u32);
                text.push(char::REPLACEMENT_CHARACTER);
            }
        }
        Self::decoded(path, text, invalid_utf8, utf16)
    }

    fn decoded(
        path: PathBuf,
        text: String,
        invalid_utf8: Vec<u32>,
        utf16: bool,
    ) -> Result<Self, FileTooLarge> {
        if u32::try_from(text.len()).is_err() {
            return Err(FileTooLarge { path });
        }
        let after_newlines = text
            .bytes()
            .enumerate()
            .filter(|&(_, byte)| byte == b'\n')
            .map(|(offset, _)| offset as u32 + 1);
        let line_starts = std::iter::once(0).chain(after_newlines).collect();
        Ok(Self {
            path,
            text,
            line_starts,
            invalid_utf8,
            utf16,
        })
    }

    /// Offsets in the text of the U+FFFD that stand for invalid UTF-8, in order.
    pub fn invalid_utf8(&self) -> &[u32] {
        &self.invalid_utf8
    }

    /// Whether the bytes began with a UTF-16 byte order mark.
    pub fn is_utf16(&self) -> bool {
        self.utf16
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn text(&self) -> &str {
        &self.text
    }

    pub fn line_col(&self, offset: u32) -> LineCol {
        let line = self.line_starts.partition_point(|&start| start <= offset);
        LineCol {
            line: line as u32,
            column: offset - self.line_starts[line - 1] + 1,
        }
    }
}

/// The files of one compilation. Files are only ever added, so a [`FileId`]
/// stays valid, and threads can add and read at the same time.
#[derive(Debug, Default)]
pub struct SourceMap {
    files: RwLock<Vec<Arc<SourceFile>>>,
}

impl SourceMap {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn add(&self, file: SourceFile) -> FileId {
        let mut files = self.files.write().unwrap_or_else(PoisonError::into_inner);
        let id = FileId(files.len() as u32);
        files.push(Arc::new(file));
        id
    }

    pub fn file(&self, id: FileId) -> Arc<SourceFile> {
        let files = self.files.read().unwrap_or_else(PoisonError::into_inner);
        Arc::clone(&files[id.0 as usize])
    }

    /// Where `span` starts, as `path:line:column`.
    pub fn location(&self, span: Span) -> String {
        let file = self.file(span.file);
        let LineCol { line, column } = file.line_col(span.lo);
        format!("{}:{line}:{column}", file.path().display())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file(text: &str) -> SourceFile {
        SourceFile::new("main.gopp".into(), text.to_owned()).unwrap()
    }

    fn line_col(text: &str, offset: usize) -> (u32, u32) {
        let LineCol { line, column } = file(text).line_col(offset as u32);
        (line, column)
    }

    #[test]
    fn offsets_map_to_lines_and_byte_columns() {
        let text = "package main\n\nfunc main() {}\n";
        assert_eq!(line_col(text, 0), (1, 1));
        assert_eq!(line_col(text, 8), (1, 9));
        assert_eq!(line_col(text, 12), (1, 13));
        assert_eq!(line_col(text, 13), (2, 1));
        assert_eq!(line_col(text, 14), (3, 1));
        assert_eq!(line_col(text, text.len()), (4, 1));
    }

    #[test]
    fn carriage_return_stays_on_its_line() {
        let text = "a\r\nb\r\n";
        assert_eq!(line_col(text, 1), (1, 2));
        assert_eq!(line_col(text, 2), (1, 3));
        assert_eq!(line_col(text, 3), (2, 1));
    }

    #[test]
    fn tabs_and_multi_byte_runes_count_bytes() {
        let text = "\tπ := 1";
        assert_eq!(line_col(text, 1), (1, 2));
        assert_eq!(line_col(text, text.find(":=").unwrap()), (1, 5));
    }

    #[test]
    fn empty_file_has_one_line() {
        assert_eq!(line_col("", 0), (1, 1));
    }

    #[test]
    fn end_of_a_file_without_final_newline() {
        assert_eq!(line_col("a\nbc", 4), (2, 3));
    }

    #[test]
    fn invalid_utf8_becomes_listed_replacement_characters() {
        let bytes = b"a\xFFb\xC3(\xE2\x82\xAC\n";
        let file = SourceFile::from_bytes("main.gopp".into(), bytes).unwrap();
        assert_eq!(file.text(), "a\u{FFFD}b\u{FFFD}(\u{20AC}\n");
        assert_eq!(file.invalid_utf8(), [1, 5]);
        assert!(!file.is_utf16());
    }

    #[test]
    fn valid_utf8_is_kept_as_it_is() {
        let text = "x := \"\u{FFFD}\"\n";
        let file = SourceFile::from_bytes("main.gopp".into(), text.as_bytes());
        let file = file.unwrap();
        assert_eq!(file.text(), text);
        assert!(file.invalid_utf8().is_empty());
    }

    #[test]
    fn a_utf16_byte_order_mark_is_noticed() {
        for bytes in [b"\xFF\xFEp\0".as_slice(), b"\xFE\xFF\0p"] {
            let file = SourceFile::from_bytes("main.gopp".into(), bytes);
            assert!(file.unwrap().is_utf16());
        }
    }

    #[test]
    fn a_source_file_ends_in_gopp() {
        assert!(is_source_path(Path::new("a/main.gopp")));
        assert!(!is_source_path(Path::new("main.go")));
        assert!(!is_source_path(Path::new("main.c")));
        assert!(!is_source_path(Path::new("gopp")));
    }

    #[test]
    fn a_span_prints_as_path_line_column() {
        let map = SourceMap::new();
        let first = map.add(file("package main\n"));
        let second = map.add(file(&format!("{}        x := 1\n", "\n".repeat(11))));
        assert_ne!(first, second);
        let span = Span {
            file: second,
            lo: 19,
            hi: 20,
        };
        assert_eq!(map.location(span), "main.gopp:12:9");
    }

    #[test]
    fn threads_add_and_read_files_together() {
        let map = SourceMap::new();
        std::thread::scope(|scope| {
            for thread in 0..8 {
                let map = &map;
                scope.spawn(move || {
                    for line in 0..100 {
                        let text = format!("// {thread} {line}\n");
                        let id = map.add(file(&text));
                        assert_eq!(map.file(id).text(), text);
                    }
                });
            }
        });
        assert_eq!(map.files.read().unwrap().len(), 800);
    }
}
