//! What happens between a checked package and what Clang makes of it
//! (pipelines.md §2): the one place where the settings of a build
//! become what each stage does, and the times of the stages.

use std::fmt;
use std::time::Duration;

use crate::driver::OptLevel;

/// What a build is asked for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PipelineRequest {
    pub opt: OptLevel,
    /// What is built says where its code is in the source, for a
    /// debugger.
    pub debug: bool,
}

/// What each stage does for a request.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PipelineSpec {
    /// `opt0` to `optz`.
    pub name: &'static str,
    /// The calls of small functions are replaced by their bodies. Not
    /// for a debugger: a call that is replaced has no frame.
    pub inline: bool,
    /// How much Clang optimizes.
    pub clang: OptLevel,
    pub debug: bool,
}

/// The pipeline of a request. A program means the same in every one.
pub fn select_pipeline(request: PipelineRequest) -> PipelineSpec {
    let name = match request.opt {
        OptLevel::O0 => "opt0",
        OptLevel::O1 => "opt1",
        OptLevel::O2 => "opt2",
        OptLevel::O3 => "opt3",
        OptLevel::Os => "opts",
        OptLevel::Oz => "optz",
    };
    PipelineSpec {
        name,
        inline: !request.debug,
        clang: request.opt,
        debug: request.debug,
    }
}

/// A stage of a build, in the order in which they run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    /// The files are read, parsed and checked.
    Check,
    /// The packages are lowered to GIR, and each module is verified.
    GirLower,
    GirInline,
    C23Emit,
    ClangCompile,
}

impl Stage {
    pub fn name(self) -> &'static str {
        match self {
            Self::Check => "check",
            Self::GirLower => "gir-lower",
            Self::GirInline => "gir-inline",
            Self::C23Emit => "c23-emit",
            Self::ClangCompile => "clang-compile",
        }
    }
}

/// How long a stage took.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StageTime {
    pub stage: Stage,
    /// When the stage started, from the start of the build.
    pub start: Duration,
    pub wall: Duration,
    /// The time of the processor that the build and what it started
    /// used: none where the system does not say it.
    pub cpu: Option<Duration>,
}

/// The times of the stages of a build.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PipelineTimes {
    pub stages: Vec<StageTime>,
}

impl fmt::Display for PipelineTimes {
    /// `check=0.012s/0.031s gir-lower=…`: the wall time, then the time
    /// of the processor, or `-`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for (at, time) in self.stages.iter().enumerate() {
            let between = match at {
                0 => "",
                _ => " ",
            };
            write!(
                f,
                "{between}{}={:.3}s/",
                time.stage.name(),
                time.wall.as_secs_f64()
            )?;
            match time.cpu {
                Some(cpu) => write!(f, "{:.3}s", cpu.as_secs_f64())?,
                None => f.write_str("-")?,
            }
        }
        Ok(())
    }
}

impl PipelineTimes {
    /// The stages as a trace that Chrome and Perfetto read: one span
    /// for each, in microseconds. `more` are the events of other
    /// tools, each a JSON object, which are added as they are.
    pub fn chrome_trace(&self, more: &[String]) -> String {
        let spans = self.stages.iter().map(|time| {
            format!(
                "{{\"name\":\"{}\",\"cat\":\"gopp\",\"ph\":\"X\",\"ts\":{},\"dur\":{},\"pid\":1,\"tid\":1}}",
                time.stage.name(),
                time.start.as_micros(),
                time.wall.as_micros()
            )
        });
        let events: Vec<String> = spans.chain(more.iter().cloned()).collect();
        format!("{{\"traceEvents\":[\n{}\n]}}\n", events.join(",\n"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_level_has_its_pipeline_and_a_debugger_keeps_the_calls() {
        let levels = [
            (OptLevel::O0, "opt0"),
            (OptLevel::O1, "opt1"),
            (OptLevel::O2, "opt2"),
            (OptLevel::O3, "opt3"),
            (OptLevel::Os, "opts"),
            (OptLevel::Oz, "optz"),
        ];
        for (opt, name) in levels {
            for debug in [false, true] {
                let spec = select_pipeline(PipelineRequest { opt, debug });
                let expected = PipelineSpec {
                    name,
                    inline: !debug,
                    clang: opt,
                    debug,
                };
                assert_eq!(spec, expected);
            }
        }
    }

    #[test]
    fn the_times_are_written_as_a_line_and_as_a_trace() {
        let millis = Duration::from_millis;
        let times = PipelineTimes {
            stages: vec![
                StageTime {
                    stage: Stage::Check,
                    start: millis(0),
                    wall: millis(12),
                    cpu: Some(millis(31)),
                },
                StageTime {
                    stage: Stage::ClangCompile,
                    start: millis(12),
                    wall: millis(1500),
                    cpu: None,
                },
            ],
        };
        assert_eq!(
            times.to_string(),
            "check=0.012s/0.031s clang-compile=1.500s/-"
        );
        let trace = times.chrome_trace(&["{\"name\":\"Frontend\"}".to_owned()]);
        let expected = "{\"traceEvents\":[\n\
            {\"name\":\"check\",\"cat\":\"gopp\",\"ph\":\"X\",\"ts\":0,\"dur\":12000,\"pid\":1,\"tid\":1},\n\
            {\"name\":\"clang-compile\",\"cat\":\"gopp\",\"ph\":\"X\",\"ts\":12000,\"dur\":1500000,\"pid\":1,\"tid\":1},\n\
            {\"name\":\"Frontend\"}\n]}\n";
        assert_eq!(trace, expected);
    }
}
