//! Subprocesses with a wall-time and a file-size bound.

use std::ffi::OsStr;
use std::process::Command;
use std::time::Duration;

/// Exit status of a [`bounded_command`] that ran out of time.
pub const TIMED_OUT: i32 = 124;

/// A command that runs `program` for at most `wall` and lets it write at most
/// `max_file_bytes` to any one file. Arguments added to the result go to
/// `program`.
pub fn bounded_command(program: impl AsRef<OsStr>, wall: Duration, max_file_bytes: u64) -> Command {
    // POSIX `ulimit -f` counts 512-byte blocks.
    let blocks = max_file_bytes.div_ceil(512);
    let mut command = Command::new("timeout");
    command
        .arg("--kill-after=1")
        .arg(wall.as_secs_f64().to_string())
        .args(["sh", "-c"])
        .arg(format!("ulimit -f {blocks} && exec \"$@\""))
        .arg("sh")
        .arg(program);
    command
}

#[cfg(test)]
mod tests {
    use super::*;

    const SECOND: Duration = Duration::from_secs(1);

    #[test]
    fn arguments_and_exit_status_pass_through() {
        let output = bounded_command("sh", SECOND, 1 << 20)
            .args(["-c", "echo \"$1\"; exit 7", "sh", "a b"])
            .output()
            .unwrap();
        assert_eq!(output.stdout, b"a b\n");
        assert_eq!(output.status.code(), Some(7));
    }

    #[test]
    fn a_slow_program_times_out() {
        let status = bounded_command("sleep", Duration::from_millis(100), 1 << 20)
            .arg("10")
            .status()
            .unwrap();
        assert_eq!(status.code(), Some(TIMED_OUT));
    }

    #[test]
    fn a_runaway_file_is_cut_off() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("out");
        let status = bounded_command("sh", SECOND, 4096)
            .args(["-c", "head -c 1000000 /dev/zero > \"$1\"", "sh"])
            .arg(&file)
            .status()
            .unwrap();
        assert!(!status.success());
        assert!(file.metadata().unwrap().len() <= 4096);
    }
}
