//! The times of the stages of a build (pipelines.md §2), for
//! `-pipeline-times` and `-trace`.

// The time of the processor is asked of the system.
#![allow(unsafe_code)]

use std::path::Path;
use std::time::{Duration, Instant};

use gopp_codegen_c::pipeline::{PipelineTimes, Stage, StageTime};
use serde_json::Value;

/// The time of the processor that this process and the processes that
/// it waited for have used: none where the system does not say it.
#[cfg(target_os = "linux")]
fn cpu() -> Option<Duration> {
    let of = |who: libc::c_int| {
        let mut usage = std::mem::MaybeUninit::<libc::rusage>::zeroed();
        // SAFETY: `usage` is room for what the system writes, and it is
        // read only after the system said that it wrote it.
        let usage = unsafe {
            if libc::getrusage(who, usage.as_mut_ptr()) != 0 {
                return None;
            }
            usage.assume_init()
        };
        let spent = |time: libc::timeval| {
            Duration::new(time.tv_sec.max(0) as u64, time.tv_usec.max(0) as u32 * 1000)
        };
        Some(spent(usage.ru_utime) + spent(usage.ru_stime))
    };
    Some(of(libc::RUSAGE_SELF)? + of(libc::RUSAGE_CHILDREN)?)
}

#[cfg(not(target_os = "linux"))]
fn cpu() -> Option<Duration> {
    None
}

/// A stage that has started.
pub struct Running {
    at: Instant,
    cpu: Option<Duration>,
}

/// What measures the stages of one build.
pub struct Clock {
    origin: Instant,
    pub times: PipelineTimes,
}

impl Clock {
    pub fn new() -> Self {
        Self {
            origin: Instant::now(),
            times: PipelineTimes::default(),
        }
    }

    pub fn begin(&self) -> Running {
        Running {
            at: Instant::now(),
            cpu: cpu(),
        }
    }

    pub fn end(&mut self, stage: Stage, running: Running) {
        let spent = cpu().zip(running.cpu);
        self.times.stages.push(StageTime {
            stage,
            start: running.at.duration_since(self.origin),
            wall: running.at.elapsed(),
            cpu: spent.map(|(now, then)| now.saturating_sub(then)),
        });
    }

    /// Writes the stages to the file as a trace that Chrome reads, with
    /// what Clang wrote of its own time into the files of `clang`, a
    /// directory. Its events start where the stage of Clang does.
    pub fn write_trace(&self, file: &Path, clang: &Path) -> std::io::Result<()> {
        let stages = &self.times.stages;
        let compiling = stages.iter().find(|time| time.stage == Stage::ClangCompile);
        let offset = compiling.map_or(0, |time| time.start.as_micros() as u64);
        let mut more = Vec::new();
        let mut reports: Vec<_> = std::fs::read_dir(clang)
            .map(|entries| entries.flatten().map(|entry| entry.path()).collect())
            .unwrap_or_default();
        reports.sort();
        for (at, report) in reports.iter().enumerate() {
            let read = std::fs::read_to_string(report).ok();
            let parsed = read.and_then(|text| serde_json::from_str::<Value>(&text).ok());
            let events = parsed.and_then(|mut report| match report["traceEvents"].take() {
                Value::Array(events) => Some(events),
                _ => None,
            });
            for mut event in events.unwrap_or_default() {
                if let Some(start) = event["ts"].as_u64() {
                    event["ts"] = Value::from(start + offset);
                }
                // The process of Clang for each unit, after this one.
                event["pid"] = Value::from(at + 2);
                more.push(event.to_string());
            }
        }
        std::fs::write(file, self.times.chrome_trace(&more))
    }
}
