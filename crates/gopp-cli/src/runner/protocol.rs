//! The lines of JSON that the test runner reads and writes
//! (test-runner.md): one job a line in, one result a line out.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

/// The version of the lines. A reader refuses another one, and passes
/// over a field that it does not know.
pub const VERSION: u32 = 1;

/// A program to build and run.
#[derive(Clone, Debug, Deserialize)]
pub struct JobSpec {
    pub v: u32,
    /// What the result is found by.
    pub id: String,
    /// One package: a directory, or files. One file of C is compiled as
    /// it is, after the header of the runtime.
    pub inputs: Vec<PathBuf>,
    #[serde(default = "host")]
    pub target: String,
    /// 0 for the time that the command line gives every job.
    #[serde(default)]
    pub timeout_ms: u64,
    /// What goes to the standard error stream goes to the standard
    /// output, in the order in which it is written.
    #[serde(default)]
    pub merge_output: bool,
}

fn host() -> String {
    "host".to_owned()
}

/// How a job ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Outcome {
    /// The program returned from `main`, or ended itself: a panic ends
    /// with status 2.
    Exited,
    /// A signal ended the program.
    Signaled,
    /// The program ran longer than it may and was killed.
    Timeout,
    /// The package did not compile.
    CompileFailed,
    /// The module of the program names what nothing defines.
    LoadFailed,
    /// The runner itself failed.
    Internal,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct JobResult {
    pub v: u32,
    pub id: String,
    pub outcome: Outcome,
    pub exit: i32,
    pub signal: i32,
    /// What the program wrote, in base64.
    pub stdout: String,
    pub stderr: String,
    /// For a job that did not run: why.
    pub message: String,
    pub compile_ms: f64,
    pub run_ms: f64,
    /// The process that ran the program did not run another after it.
    pub restarted: bool,
}

impl JobResult {
    /// The result of a job that did not run.
    pub fn without_run(id: &str, outcome: Outcome, message: String) -> Self {
        Self {
            v: VERSION,
            id: id.to_owned(),
            outcome,
            exit: 0,
            signal: 0,
            stdout: String::new(),
            stderr: String::new(),
            message,
            compile_ms: 0.0,
            run_ms: 0.0,
            restarted: false,
        }
    }
}

/// The job of a line of a manifest.
pub fn job_of(line: &str) -> Result<JobSpec, String> {
    let spec: JobSpec = serde_json::from_str(line).map_err(|error| error.to_string())?;
    match spec.v {
        VERSION => Ok(spec),
        other => Err(format!(
            "version {other} of a job, this runner reads {VERSION}"
        )),
    }
}

const BASE64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// The bytes in base64, with padding.
pub fn base64(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let mut group = [0u8; 3];
        group[..chunk.len()].copy_from_slice(chunk);
        let bits = u32::from_be_bytes([0, group[0], group[1], group[2]]);
        for at in 0..4 {
            let digit = BASE64[(bits >> (18 - 6 * at) & 63) as usize] as char;
            text.push(if at <= chunk.len() { digit } else { '=' });
        }
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bytes_are_written_in_groups_of_four_letters() {
        let cases = [
            ("", ""),
            ("f", "Zg=="),
            ("fo", "Zm8="),
            ("foo", "Zm9v"),
            ("foob", "Zm9vYg=="),
            ("fooba", "Zm9vYmE="),
            ("foobar", "Zm9vYmFy"),
        ];
        for (bytes, text) in cases {
            assert_eq!(base64(bytes.as_bytes()), text);
        }
        assert_eq!(base64(&[0xfb, 0xff, 0xbe]), "+/++");
    }

    #[test]
    fn a_job_has_defaults_and_fields_that_are_not_known_are_passed_over() {
        let spec = job_of(r#"{"v":1,"id":"a","inputs":["a.gopp"],"later":[1,2]}"#).unwrap();
        assert_eq!((spec.id.as_str(), spec.target.as_str()), ("a", "host"));
        assert_eq!((spec.timeout_ms, spec.merge_output), (0, false));
        let full = r#"{"v":1,"id":"b","inputs":["d"],"target":"linux/amd64","timeout_ms":50,"merge_output":true}"#;
        let spec = job_of(full).unwrap();
        assert_eq!((spec.timeout_ms, spec.merge_output), (50, true));
        let refused = job_of(r#"{"v":2,"id":"a","inputs":[]}"#).unwrap_err();
        assert_eq!(refused, "version 2 of a job, this runner reads 1");
        assert!(job_of(r#"{"v":1,"inputs":[]}"#).is_err());
    }

    #[test]
    fn a_result_is_one_line_with_the_names_of_the_design() {
        let mut result = JobResult::without_run("core-flow", Outcome::CompileFailed, "no".into());
        result.compile_ms = 3.5;
        assert_eq!(
            serde_json::to_string(&result).unwrap(),
            r#"{"v":1,"id":"core-flow","outcome":"compile-failed","exit":0,"signal":0,"stdout":"","stderr":"","message":"no","compile_ms":3.5,"run_ms":0.0,"restarted":false}"#
        );
        let names = [Outcome::Exited, Outcome::Signaled, Outcome::Timeout];
        let written: Vec<String> = names
            .iter()
            .chain(&[Outcome::LoadFailed, Outcome::Internal])
            .map(|outcome| serde_json::to_string(outcome).unwrap())
            .collect();
        let expected = [
            "\"exited\"",
            "\"signaled\"",
            "\"timeout\"",
            "\"load-failed\"",
            "\"internal\"",
        ];
        assert_eq!(written, expected);
    }
}
