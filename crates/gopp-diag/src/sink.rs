//! Where one compilation job collects its diagnostics.

use std::sync::{Mutex, PoisonError};

use crate::{Code, Diagnostic};

/// Collects diagnostics from any number of threads and hands them back in
/// source order, so the output never depends on which thread reported first.
///
/// At most `cap` diagnostics are kept: the first ones in source order, then
/// one [`Code::TooManyErrors`].
#[derive(Debug)]
pub struct Sink {
    cap: usize,
    state: Mutex<State>,
}

#[derive(Debug, Default)]
struct State {
    kept: Vec<Diagnostic>,
    dropped: bool,
}

impl Sink {
    pub fn new(cap: usize) -> Self {
        Self {
            cap,
            state: Mutex::default(),
        }
    }

    pub fn push(&self, diagnostic: Diagnostic) {
        let mut state = self.state.lock().unwrap_or_else(PoisonError::into_inner);
        state.kept.push(diagnostic);
        // Trimming only when the buffer is twice the cap keeps pushes cheap
        // and memory bounded.
        if state.kept.len() >= 2 * self.cap.max(1) {
            state.trim(self.cap);
        }
    }

    /// The kept diagnostics, sorted by file, offset, then code.
    pub fn finish(self) -> Vec<Diagnostic> {
        let mut state = self
            .state
            .into_inner()
            .unwrap_or_else(PoisonError::into_inner);
        state.trim(self.cap);
        if state.dropped
            && let Some(last) = state.kept.last()
        {
            let span = last.span;
            state.kept.push(Diagnostic::new(
                Code::TooManyErrors,
                span,
                "too many errors",
            ));
        }
        state.kept
    }
}

impl State {
    fn trim(&mut self, cap: usize) {
        self.kept.sort_by(|a, b| {
            (a.span.file, a.span.lo, a.code, a.span.hi, &a.message).cmp(&(
                b.span.file,
                b.span.lo,
                b.code,
                b.span.hi,
                &b.message,
            ))
        });
        self.dropped |= self.kept.len() > cap;
        self.kept.truncate(cap);
    }
}

#[cfg(test)]
mod tests {
    use gopp_span::{Dialect, FileId, SourceFile, SourceMap, Span};

    use super::*;

    fn file_ids(count: usize) -> Vec<FileId> {
        let map = SourceMap::new();
        (0..count)
            .map(|_| {
                let file = SourceFile::new("f.gopp".into(), String::new(), Dialect::Gopp);
                map.add(file.unwrap())
            })
            .collect()
    }

    fn at(file: FileId, lo: u32, code: Code) -> Diagnostic {
        let span = Span {
            file,
            lo,
            hi: lo + 1,
        };
        Diagnostic::new(code, span, format!("at {lo}"))
    }

    #[test]
    fn diagnostics_come_back_by_file_offset_then_code() {
        let files = file_ids(2);
        let sink = Sink::new(10);
        sink.push(at(files[1], 0, Code::LexIllegalChar));
        sink.push(at(files[0], 7, Code::LexIllegalChar));
        sink.push(at(files[0], 7, Code::LexIllegalNul));
        sink.push(at(files[0], 3, Code::LexNoDigits));
        let order: Vec<_> = sink
            .finish()
            .iter()
            .map(|d| (d.span.file, d.span.lo, d.code))
            .collect();
        assert_eq!(
            order,
            [
                (files[0], 3, Code::LexNoDigits),
                (files[0], 7, Code::LexIllegalNul),
                (files[0], 7, Code::LexIllegalChar),
                (files[1], 0, Code::LexIllegalChar),
            ]
        );
    }

    #[test]
    fn the_cap_keeps_the_first_in_source_order_and_says_so_once() {
        let file = file_ids(1)[0];
        let sink = Sink::new(3);
        for lo in (0..100).rev() {
            sink.push(at(file, lo, Code::LexIllegalChar));
        }
        let kept = sink.finish();
        let offsets: Vec<_> = kept.iter().map(|d| d.span.lo).collect();
        assert_eq!(offsets, [0, 1, 2, 2]);
        assert_eq!(kept[3].code, Code::TooManyErrors);
        assert_eq!(kept[3].message, "too many errors");
    }

    #[test]
    fn exactly_the_cap_is_not_too_many() {
        let file = file_ids(1)[0];
        let sink = Sink::new(3);
        for lo in 0..3 {
            sink.push(at(file, lo, Code::LexIllegalChar));
        }
        assert_eq!(sink.finish().len(), 3);
    }

    #[test]
    fn the_result_does_not_depend_on_which_thread_pushed_first() {
        let files = file_ids(4);
        let run = |threads: usize| {
            let sink = Sink::new(50);
            std::thread::scope(|scope| {
                for thread in 0..threads {
                    let (sink, files) = (&sink, &files);
                    scope.spawn(move || {
                        for lo in (thread as u32..400).step_by(threads) {
                            sink.push(at(files[lo as usize % 4], lo, Code::LexIllegalChar));
                        }
                    });
                }
            });
            sink.finish()
        };
        let single = run(1);
        assert_eq!(single.len(), 51);
        for _ in 0..20 {
            assert_eq!(run(8), single);
        }
    }
}
