//===- Protocol.cpp - JSON lines of the test runner -----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/TestRun/Protocol.h"

#include "llvm/Support/Base64.h"
#include "llvm/Support/JSON.h"

using namespace gopp::testrun;
using llvm::json::Object;
using llvm::json::Value;

const char *gopp::testrun::outcomeName(Outcome O) {
  switch (O) {
  case Outcome::Exited:
    return "exited";
  case Outcome::Signaled:
    return "signaled";
  case Outcome::TimedOut:
    return "timeout";
  case Outcome::CompileFailed:
    return "compile-failed";
  case Outcome::LoadFailed:
    return "load-failed";
  case Outcome::Internal:
    return "internal";
  }
  return "internal";
}

bool gopp::testrun::parseOutcome(llvm::StringRef Name, Outcome &O) {
  for (Outcome C :
       {Outcome::Exited, Outcome::Signaled, Outcome::TimedOut,
        Outcome::CompileFailed, Outcome::LoadFailed, Outcome::Internal})
    if (Name == outcomeName(C)) {
      O = C;
      return true;
    }
  return false;
}

static llvm::Error bad(const char *What) {
  return llvm::createStringError(std::errc::invalid_argument,
                                 "test-run protocol: %s", What);
}

/// The object of a line, after the version check.
static llvm::Expected<Object> parseLine(llvm::StringRef Line) {
  llvm::Expected<Value> V = llvm::json::parse(Line);
  if (!V)
    return V.takeError();
  Object *O = V->getAsObject();
  if (!O)
    return bad("a line is not a JSON object");
  std::optional<int64_t> Ver = O->getInteger("v");
  if (!Ver || *Ver != ProtocolVersion)
    return bad("unknown protocol version");
  return std::move(*O);
}

static std::string line(Object O) {
  O["v"] = int64_t(ProtocolVersion);
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << Value(std::move(O));
  return S;
}

/// Text that may hold bytes that are not UTF-8 (a path, a diagnostic).
static std::string txt(llvm::StringRef S) { return llvm::json::fixUTF8(S); }

static std::string b64(llvm::StringRef Bytes) {
  return llvm::encodeBase64(Bytes);
}

static llvm::Error unb64(const Object &O, llvm::StringRef Key,
                         std::string &Out) {
  std::optional<llvm::StringRef> S = O.getString(Key);
  if (!S)
    return llvm::Error::success(); // absent = empty
  std::vector<char> Bytes;
  if (llvm::Error E = llvm::decodeBase64(*S, Bytes))
    return E;
  Out.assign(Bytes.begin(), Bytes.end());
  return llvm::Error::success();
}

static std::string str(const Object &O, llvm::StringRef Key) {
  return O.getString(Key).value_or("").str();
}

std::string gopp::testrun::encodeJobSpec(const JobSpec &J) {
  llvm::json::Array In;
  for (const std::string &I : J.Inputs)
    In.push_back(txt(I));
  Object O;
  O["id"] = txt(J.Id);
  O["inputs"] = std::move(In);
  O["target"] = txt(J.Target);
  O["timeout_ms"] = int64_t(J.TimeoutMs);
  if (J.MergeOutput)
    O["merge_output"] = true;
  return line(std::move(O));
}

llvm::Expected<JobSpec> gopp::testrun::decodeJobSpec(llvm::StringRef Line) {
  llvm::Expected<Object> O = parseLine(Line);
  if (!O)
    return O.takeError();
  JobSpec J;
  J.Id = str(*O, "id");
  if (J.Id.empty())
    return bad("a job has no id");
  llvm::json::Array *In = O->getArray("inputs");
  if (!In || In->empty())
    return bad("a job has no inputs");
  for (const Value &V : *In) {
    std::optional<llvm::StringRef> S = V.getAsString();
    if (!S)
      return bad("an input is not a string");
    J.Inputs.push_back(S->str());
  }
  if (O->getString("target"))
    J.Target = str(*O, "target");
  J.TimeoutMs = uint32_t(O->getInteger("timeout_ms").value_or(0));
  J.MergeOutput = O->getBoolean("merge_output").value_or(false);
  return J;
}

std::string gopp::testrun::encodeJobResult(const JobResult &R) {
  Object O;
  O["id"] = txt(R.Id);
  O["outcome"] = outcomeName(R.O);
  O["exit"] = int64_t(R.ExitCode);
  O["signal"] = int64_t(R.Signal);
  O["stdout"] = b64(R.Stdout);
  O["stderr"] = b64(R.Stderr);
  O["message"] = txt(R.Message);
  O["compile_ms"] = R.CompileMs;
  O["run_ms"] = R.RunMs;
  O["restarted"] = R.ExecutorRestarted;
  return line(std::move(O));
}

llvm::Expected<JobResult> gopp::testrun::decodeJobResult(llvm::StringRef Line) {
  llvm::Expected<Object> O = parseLine(Line);
  if (!O)
    return O.takeError();
  JobResult R;
  R.Id = str(*O, "id");
  if (R.Id.empty() || !parseOutcome(str(*O, "outcome"), R.O))
    return bad("a result has no id or a bad outcome");
  R.ExitCode = int(O->getInteger("exit").value_or(0));
  R.Signal = int(O->getInteger("signal").value_or(0));
  if (llvm::Error E = unb64(*O, "stdout", R.Stdout))
    return std::move(E);
  if (llvm::Error E = unb64(*O, "stderr", R.Stderr))
    return std::move(E);
  R.Message = str(*O, "message");
  R.CompileMs = O->getNumber("compile_ms").value_or(0);
  R.RunMs = O->getNumber("run_ms").value_or(0);
  R.ExecutorRestarted = O->getBoolean("restarted").value_or(false);
  return R;
}

std::string gopp::testrun::encodeRunRequest(const RunRequest &R) {
  Object O;
  O["cmd"] = "run";
  O["id"] = txt(R.Id);
  O["object"] = txt(R.ObjectPath);
  O["stdout"] = txt(R.StdoutPath);
  O["stderr"] = txt(R.StderrPath);
  O["max_output"] = int64_t(R.MaxOutputBytes);
  return line(std::move(O));
}

llvm::Expected<RunRequest>
gopp::testrun::decodeRunRequest(llvm::StringRef Line) {
  llvm::Expected<Object> O = parseLine(Line);
  if (!O)
    return O.takeError();
  if (str(*O, "cmd") != "run")
    return bad("not a run request");
  RunRequest R;
  R.Id = str(*O, "id");
  R.ObjectPath = str(*O, "object");
  R.StdoutPath = str(*O, "stdout");
  R.StderrPath = str(*O, "stderr");
  R.MaxOutputBytes = uint64_t(O->getInteger("max_output").value_or(0));
  if (R.ObjectPath.empty() || R.StdoutPath.empty())
    return bad("a run request lacks a path");
  return R;
}

std::string gopp::testrun::encodeRunReply(const RunReply &R) {
  Object O;
  O["event"] = "done";
  O["id"] = txt(R.Id);
  O["loaded"] = R.Loaded;
  O["exit"] = int64_t(R.ExitCode);
  O["error"] = txt(R.Error);
  if (R.Recycle)
    O["recycle"] = true;
  return line(std::move(O));
}

llvm::Expected<RunReply> gopp::testrun::decodeRunReply(llvm::StringRef Line) {
  llvm::Expected<Object> O = parseLine(Line);
  if (!O)
    return O.takeError();
  if (str(*O, "event") != "done")
    return bad("not a run reply");
  RunReply R;
  R.Id = str(*O, "id");
  R.Loaded = O->getBoolean("loaded").value_or(false);
  R.ExitCode = int(O->getInteger("exit").value_or(0));
  R.Error = str(*O, "error");
  R.Recycle = O->getBoolean("recycle").value_or(false);
  return R;
}

std::string gopp::testrun::encodeReady() {
  Object O;
  O["event"] = "ready";
  return line(std::move(O));
}

bool gopp::testrun::isReady(llvm::StringRef Line) {
  llvm::Expected<Object> O = parseLine(Line);
  if (!O) {
    llvm::consumeError(O.takeError());
    return false;
  }
  return str(*O, "event") == "ready";
}

std::string gopp::testrun::encodeQuit() {
  Object O;
  O["cmd"] = "quit";
  return line(std::move(O));
}

bool gopp::testrun::isQuit(llvm::StringRef Line) {
  llvm::Expected<Object> O = parseLine(Line);
  if (!O) {
    llvm::consumeError(O.takeError());
    return false;
  }
  return str(*O, "cmd") == "quit";
}
