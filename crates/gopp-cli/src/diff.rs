//! A line diff in the unified format.

use std::fmt::Write;

const CONTEXT: usize = 3;
/// Above this many table cells the changed region is shown as one
/// replacement.
const MAX_CELLS: usize = 1 << 24;

#[derive(Clone, Copy, PartialEq)]
enum Edit {
    Keep,
    Delete,
    Insert,
}

/// The edits that turn `old` into `new`: a longest common subsequence
/// between the lines the two do not share at their start and end.
fn edits(old: &[&str], new: &[&str]) -> Vec<Edit> {
    let shared = |a: &&str, b: &&str| a == b;
    let prefix = old
        .iter()
        .zip(new)
        .take_while(|(a, b)| shared(a, b))
        .count();
    let (old_rest, new_rest) = (&old[prefix..], &new[prefix..]);
    let suffix = old_rest
        .iter()
        .rev()
        .zip(new_rest.iter().rev())
        .take_while(|(a, b)| shared(a, b))
        .count();
    let old_mid = &old_rest[..old_rest.len() - suffix];
    let new_mid = &new_rest[..new_rest.len() - suffix];

    let mut edits = vec![Edit::Keep; prefix];
    let (rows, columns) = (old_mid.len(), new_mid.len());
    if rows.saturating_mul(columns) > MAX_CELLS {
        edits.extend(std::iter::repeat_n(Edit::Delete, rows));
        edits.extend(std::iter::repeat_n(Edit::Insert, columns));
    } else {
        // `longest[i][j]`: the common lines of `old_mid[i..]` and `new_mid[j..]`.
        let width = columns + 1;
        let mut longest = vec![0u32; (rows + 1) * width];
        for i in (0..rows).rev() {
            for j in (0..columns).rev() {
                longest[i * width + j] = if old_mid[i] == new_mid[j] {
                    longest[(i + 1) * width + j + 1] + 1
                } else {
                    longest[(i + 1) * width + j].max(longest[i * width + j + 1])
                };
            }
        }
        let (mut i, mut j) = (0, 0);
        while i < rows || j < columns {
            if i < rows && j < columns && old_mid[i] == new_mid[j] {
                edits.push(Edit::Keep);
                (i, j) = (i + 1, j + 1);
            } else if j == columns
                || i < rows && longest[(i + 1) * width + j] >= longest[i * width + j + 1]
            {
                edits.push(Edit::Delete);
                i += 1;
            } else {
                edits.push(Edit::Insert);
                j += 1;
            }
        }
    }
    edits.extend(std::iter::repeat_n(Edit::Keep, suffix));
    edits
}

/// The unified diff of two texts, empty if they are equal.
pub fn unified(old_name: &str, old: &str, new_name: &str, new: &str) -> String {
    if old == new {
        return String::new();
    }
    let old_lines: Vec<&str> = old.split_inclusive('\n').collect();
    let new_lines: Vec<&str> = new.split_inclusive('\n').collect();
    let edits = edits(&old_lines, &new_lines);

    let mut out = format!("diff {old_name} {new_name}\n--- {old_name}\n+++ {new_name}\n");
    let changed: Vec<usize> = (0..edits.len())
        .filter(|&index| edits[index] != Edit::Keep)
        .collect();
    let mut first = 0;
    // Line numbers of the edit at `at`, counted as the hunks are written.
    let (mut at, mut old_line, mut new_line) = (0, 0, 0);
    while first < changed.len() {
        // Changes closer than two contexts share a hunk.
        let mut last = first;
        while last + 1 < changed.len() && changed[last + 1] - changed[last] <= 2 * CONTEXT {
            last += 1;
        }
        let start = changed[first].saturating_sub(CONTEXT);
        let end = (changed[last] + CONTEXT + 1).min(edits.len());
        for edit in &edits[at..start] {
            old_line += usize::from(*edit != Edit::Insert);
            new_line += usize::from(*edit != Edit::Delete);
        }
        let hunk = &edits[start..end];
        let old_count = hunk.iter().filter(|&&edit| edit != Edit::Insert).count();
        let new_count = hunk.iter().filter(|&&edit| edit != Edit::Delete).count();
        // An empty side is numbered by the line before it.
        let old_start = old_line + usize::from(old_count > 0);
        let new_start = new_line + usize::from(new_count > 0);
        // Writing to a String cannot fail.
        let _ = writeln!(
            out,
            "@@ -{old_start},{old_count} +{new_start},{new_count} @@"
        );
        for edit in hunk {
            let (mark, line) = match edit {
                Edit::Keep => (' ', old_lines[old_line]),
                Edit::Delete => ('-', old_lines[old_line]),
                Edit::Insert => ('+', new_lines[new_line]),
            };
            old_line += usize::from(*edit != Edit::Insert);
            new_line += usize::from(*edit != Edit::Delete);
            out.push(mark);
            out.push_str(line);
            if !line.ends_with('\n') {
                out.push_str("\n\\ No newline at end of file\n");
            }
        }
        at = end;
        first = last + 1;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn equal_texts_have_no_diff() {
        assert_eq!(unified("a", "x\n", "b", "x\n"), "");
    }

    #[test]
    fn a_changed_line_is_shown_with_its_context() {
        let old = "1\n2\n3\n4\n5\n6\n7\n8\n9\n";
        let new = "1\n2\n3\n4\nfive\n6\n7\n8\n9\n";
        assert_eq!(
            unified("f.orig", old, "f", new),
            "diff f.orig f\n--- f.orig\n+++ f\n@@ -2,7 +2,7 @@\n 2\n 3\n 4\n-5\n+five\n 6\n 7\n 8\n"
        );
    }

    #[test]
    fn changes_far_apart_get_a_hunk_each() {
        let old: String = (1..=20).map(|n| format!("{n}\n")).collect();
        let new = old.replace("2\n3\n", "2\n").replace("19\n", "19\nnew\n");
        let diff = unified("a", &old, "b", &new);
        assert!(
            diff.contains("@@ -1,6 +1,5 @@\n 1\n 2\n-3\n 4\n 5\n 6\n"),
            "{diff}"
        );
        assert!(
            diff.contains("@@ -17,4 +16,5 @@\n 17\n 18\n 19\n+new\n 20\n"),
            "{diff}"
        );
    }

    #[test]
    fn a_missing_final_newline_is_marked() {
        assert_eq!(
            unified("a", "x", "b", "x\n"),
            "diff a b\n--- a\n+++ b\n@@ -1,1 +1,1 @@\n-x\n\\ No newline at end of file\n+x\n"
        );
    }
}
