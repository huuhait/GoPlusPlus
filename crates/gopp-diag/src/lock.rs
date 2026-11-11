//! The append-only registry `docs/errors/codes.lock`.

/// Why a lock file is not a valid registry. Lines are 1-based.
#[derive(Debug, PartialEq, Eq)]
pub enum LockError {
    Malformed { line: usize },
    OutOfOrder { line: usize, expected: u16 },
    DuplicateName { line: usize },
}

/// The rows of a lock file: row `n` names diagnostic `E000n`.
#[derive(Debug)]
pub struct LockRegistry {
    names: Vec<String>,
}

impl LockRegistry {
    /// Parses `E<number> <name>` rows; `#` starts a comment line. Numbers
    /// must count up from 1 without a gap, which is what makes a removed or
    /// renumbered row an error.
    pub fn parse(text: &str) -> Result<Self, LockError> {
        let mut names: Vec<String> = Vec::new();
        for (index, row) in text.lines().enumerate() {
            let line = index + 1;
            if row.starts_with('#') {
                continue;
            }
            let (number, name) = parse_row(row).ok_or(LockError::Malformed { line })?;
            let expected = names.len() as u16 + 1;
            if number != expected {
                return Err(LockError::OutOfOrder { line, expected });
            }
            if names.iter().any(|known| known == name) {
                return Err(LockError::DuplicateName { line });
            }
            names.push(name.to_owned());
        }
        Ok(Self { names })
    }

    pub fn len(&self) -> usize {
        self.names.len()
    }

    pub fn is_empty(&self) -> bool {
        self.names.is_empty()
    }

    pub fn name(&self, number: u16) -> Option<&str> {
        let index = usize::from(number).checked_sub(1)?;
        self.names.get(index).map(String::as_str)
    }
}

fn parse_row(row: &str) -> Option<(u16, &str)> {
    let (code, name) = row.split_once(' ')?;
    let digits = code.strip_prefix('E')?;
    let well_formed = digits.len() == 4
        && digits.bytes().all(|byte| byte.is_ascii_digit())
        && ["err_", "warn_", "note_"]
            .iter()
            .any(|prefix| name.len() > prefix.len() && name.starts_with(prefix))
        && name
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'_');
    if !well_formed {
        return None;
    }
    Some((digits.parse().ok()?, name))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rows_are_read_by_number() {
        let lock = LockRegistry::parse("# header\nE0001 err_a\nE0002 note_b\n").unwrap();
        assert_eq!(lock.len(), 2);
        assert_eq!(lock.name(1), Some("err_a"));
        assert_eq!(lock.name(2), Some("note_b"));
        assert_eq!(lock.name(0), None);
        assert_eq!(lock.name(3), None);
    }

    #[test]
    fn malformed_rows_are_refused() {
        for row in [
            "E001 err_a",
            "E0001  err_a",
            "E0001 err_",
            "E0001 Err_a",
            "0001 err_a",
            "E0001",
            "E0001 info_a",
            "E0001 err_a ",
        ] {
            let error = LockRegistry::parse(row).unwrap_err();
            assert_eq!(error, LockError::Malformed { line: 1 }, "{row:?}");
        }
    }

    #[test]
    fn a_removed_or_renumbered_row_is_refused() {
        let removed = LockRegistry::parse("E0001 err_a\nE0003 err_c\n").unwrap_err();
        assert_eq!(
            removed,
            LockError::OutOfOrder {
                line: 2,
                expected: 2
            }
        );
        let swapped = LockRegistry::parse("E0002 err_b\nE0001 err_a\n").unwrap_err();
        assert_eq!(
            swapped,
            LockError::OutOfOrder {
                line: 1,
                expected: 1
            }
        );
    }

    #[test]
    fn a_duplicated_row_is_refused() {
        let number = LockRegistry::parse("E0001 err_a\nE0001 err_b\n").unwrap_err();
        assert_eq!(
            number,
            LockError::OutOfOrder {
                line: 2,
                expected: 2
            }
        );
        let name = LockRegistry::parse("E0001 err_a\nE0002 err_a\n").unwrap_err();
        assert_eq!(name, LockError::DuplicateName { line: 2 });
    }
}
