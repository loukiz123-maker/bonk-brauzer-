// test262 runner.
//
// Corpus: https://github.com/tc39/test262 — 50k+ files, each a tiny program with
// a `/*--- … ---*/` metadata block. We run every file that fits inside the
// feature envelope of our engine (see kUnsupportedFeatures) and report how many
// reach the end without throwing. Tests that expect a throw (`negative:`) are
// checked for the phase (parse vs runtime) rather than the exact error class,
// because our error objects do not yet carry `name`.
//
// The harness files test262 normally prepends (`assert.js`, `sta.js`) are replaced
// by the self-contained shim below, written in the subset of JS the engine
// supports. That keeps this runner dependency-free but means a handful of
// assertion-heavy tests fail for harness reasons, not engine reasons.
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "bonk/conformance/conformance.hpp"
#include "bonk/js/js.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#define BONK_CONFORMANCE_FORK 1
#else
#define BONK_CONFORMANCE_FORK 0
#endif

namespace bonk::conformance {
namespace {

// Features the engine does not implement yet. Anything listed here is skipped,
// never counted as a pass.
const std::unordered_set<std::string>& unsupported_features() {
  static const std::unordered_set<std::string> kFeatures = {
      "Proxy",          "Reflect",         "Reflect.construct", "Reflect.defineProperty",
      "Symbol",         "Symbol.iterator", "Symbol.toStringTag", "Symbol.asyncIterator",
      "BigInt",         "class",           "class-fields-public", "class-fields-private",
      "class-methods-private", "class-static-methods-private", "class-static-fields-public",
      "generators",     "async-functions", "async-iteration",  "Promise",
      "Map",            "Set",             "WeakMap",           "WeakSet",
      "TypedArray",     "ArrayBuffer",     "DataView",          "SharedArrayBuffer",
      "Atomics",        "regexp",          "regexp-unicode-property-escapes",
      "regexp-lookbehind", "regexp-named-groups", "Temporal",   "Intl",
      "Intl.Era-monthcode", "destructuring-binding", "computed-property-names",
      "default-parameters", "spread",      "rest-parameters",   "for-of",
      "dynamic-import", "modules",         "explicit-resource-management",
      "iterator-helpers", "decorators",    "deferred-import-evaluation",
      "hashbang",       "source-phase-imports", "import-attributes",
      "tail-call-optimization", "u180e",    "FinalizationRegistry", "WeakRef",
      "coalesce-expression", "optional-chaining", "logical-assignment-operators",
      "numeric-separator-literal", "object-spread", "object-rest",
      "exponentiation", "String.prototype.matchAll", "well-formed-json-stringify",
      "Atomics.waitAsync", "change-array-by-copy", "Array.prototype.group",
      "Array.prototype.toSpliced", "Array.prototype.with", "json-parse-with-source",
      "array-find-from-last", "set-methods", "Symbol.matchAll", "legacy-regexp",
      "non-blocking-import", "String.prototype.replaceAll", "Intl.DurationFormat",
      "cross-realm",    "host-gc-required", "CanBlockIsFalse", "CanBlockIsTrue",
  };
  return kFeatures;
}

// The built-ins the engine actually provides (see src/js/interp.cpp). Everything
// else in test262 — Date, RegExp, typed arrays, Symbol, Map/Set, Promise … — is
// outside the envelope and is skipped rather than counted as a failure.
const std::unordered_set<std::string>& supported_builtins() {
  static const std::unordered_set<std::string> kBuiltins = {
      "Object", "Array",  "String", "Number", "Boolean", "Math",    "JSON",   "Error",
      "Function", "parseInt", "parseFloat", "isNaN", "global", "Array.prototype", "Object.prototype",
  };
  return kBuiltins;
}

/// Harness files whose behaviour the preamble above reproduces. Everything else
/// (`compareIterator.js`, `temporalHelpers.js`, …) keeps its tests out of the run.
const std::unordered_set<std::string>& supported_includes() {
  static const std::unordered_set<std::string> kIncludes = {
      "assert.js", "sta.js", "propertyHelper.js", "compareArray.js", "isConstructor.js",
  };
  return kIncludes;
}

/// True when the file is inside the part of the suite we claim to implement.
[[nodiscard]] bool in_envelope(const std::string& relative) {
  const std::string relative_path = "/" + relative;  // makes every check prefix-uniform
  if (relative_path.find("/annexB/") != std::string::npos) return false;
  if (relative_path.find("/intl402/") != std::string::npos) return false;
  if (relative_path.find("/language/") != std::string::npos) return true;
  const std::size_t at = relative_path.find("/built-ins/");
  if (at == std::string::npos) return false;
  const std::size_t name_begin = at + 11;
  const std::size_t name_end = relative_path.find('/', name_begin);
  if (name_end == std::string::npos) return false;
  const std::string name = relative_path.substr(name_begin, name_end - name_begin);
  return supported_builtins().count(name) != 0;
}

struct Metadata {
  std::vector<std::string> flags;
  std::vector<std::string> features;
  std::vector<std::string> includes;
  bool negative = false;
  std::string negative_phase;
  std::string negative_type;
};

[[nodiscard]] std::string trim_copy(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) {
    ++begin;
  }
  while (end > begin &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

[[nodiscard]] std::vector<std::string> parse_list(std::string_view body) {
  std::vector<std::string> out;
  const std::size_t open = body.find('[');
  const std::size_t close = body.find(']', open == std::string_view::npos ? 0 : open);
  if (open == std::string_view::npos || close == std::string_view::npos) return out;
  const std::string_view content = body.substr(open + 1, close - open - 1);
  std::size_t start = 0;
  for (;;) {
    const std::size_t comma = content.find(',', start);
    const std::string item =
        trim_copy(comma == std::string_view::npos ? content.substr(start) : content.substr(start, comma - start));
    if (!item.empty()) out.push_back(item);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return out;
}

[[nodiscard]] Metadata parse_metadata(std::string_view text) {
  Metadata metadata;
  const std::size_t begin = text.find("/*---");
  if (begin == std::string_view::npos) return metadata;
  const std::size_t end = text.find("---*/", begin);
  const std::string_view body = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);

  if (const std::size_t at = body.find("flags:"); at != std::string_view::npos) {
    metadata.flags = parse_list(body.substr(at));
  }
  if (const std::size_t at = body.find("features:"); at != std::string_view::npos) {
    metadata.features = parse_list(body.substr(at));
  }
  if (const std::size_t at = body.find("includes:"); at != std::string_view::npos) {
    metadata.includes = parse_list(body.substr(at));
  }
  const std::size_t negative_at = body.find("negative:");
  if (negative_at != std::string_view::npos) {
    metadata.negative = true;
    const std::string_view rest = body.substr(negative_at);
    const std::size_t phase_at = rest.find("phase:");
    if (phase_at != std::string_view::npos) {
      const std::string_view tail = rest.substr(phase_at + 6);
      metadata.negative_phase = trim_copy(tail.substr(0, tail.find_first_of(",\n")));
    }
    const std::size_t type_at = rest.find("type:");
    if (type_at != std::string_view::npos) {
      const std::string_view tail = rest.substr(type_at + 5);
      metadata.negative_type = trim_copy(tail.substr(0, tail.find_first_of(",\n")));
    }
  }
  return metadata;
}

[[nodiscard]] bool contains(const std::vector<std::string>& list, std::string_view value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

// The part of test262's harness we can express in our own JS subset.
//
// This is the *measurement instrument*, so it has to be honest: an assertion either
// checks what test262 checks, or the test is skipped (see supported_includes()).
// Historically `assert.throws` here was a stub that always threw, which turned every
// negative test into a failure; the version below matches the real `assert.js`.
constexpr char kPreamble[] = R"JS(
var Test262Error = function (message) {
  var error = new Error(message === undefined ? "" : message);
  error.name = "Test262Error";
  return error;
};
var $ERROR = function (m) { throw new Test262Error(m); };
var $DONOTEVALUATE = function () { throw new Test262Error("This statement should not be evaluated."); };
var $262 = { global: this, detachArrayBuffer: function () {} };
var print = function () {};

// SameValue, spelled out: NaN equals NaN, +0 and -0 differ.
var $sameValue = function (a, b) {
  if (a === b) { return a !== 0 || 1 / a === 1 / b; }
  return a !== a && b !== b;
};
// Which constructor (if any) a value is an instance of, by the standard's hierarchy.
var $isErrorClass = function (value, expected) {
  if (expected === undefined) return true;
  if (typeof expected === "function") return value instanceof expected;
  return false;
};

var assert = {};
assert.sameValue = function (actual, expected, message) {
  if (!$sameValue(actual, expected)) {
    throw new Test262Error("Expected SameValue(«" + actual + "», «" + expected + "») to be true" +
                           (message === undefined ? "" : ": " + message));
  }
};
assert.notSameValue = function (actual, expected, message) {
  if ($sameValue(actual, expected)) {
    throw new Test262Error("Expected SameValue(«" + actual + "», «" + expected + "») to be false" +
                           (message === undefined ? "" : ": " + message));
  }
};
assert.true = function (actual, message) {
  if (actual !== true) { throw new Test262Error("Expected true" + (message === undefined ? "" : ": " + message)); }
};
assert.false = function (actual, message) {
  if (actual !== false) { throw new Test262Error("Expected false" + (message === undefined ? "" : ": " + message)); }
};
assert.fail = function (message) {
  throw new Test262Error(message === undefined ? "assert.fail" : message);
};
assert.throws = function (expected, fn, message) {
  var threw = false;
  var error = undefined;
  try {
    fn();
  } catch (caught) {
    threw = true;
    error = caught;
  }
  var suffix = message === undefined ? "" : ": " + message;
  if (!threw) { throw new Test262Error("Expected a throw, but nothing was thrown" + suffix); }
  if (expected === undefined) return;
  if (typeof expected === "function") {
    if (!(error instanceof expected)) {
      throw new Test262Error("Expected a " + expected.name + ", got " + error + suffix);
    }
    return;
  }
  if (expected !== null && typeof expected === "object") {
    if (expected.name !== undefined && String(error.name) !== String(expected.name)) {
      throw new Test262Error("Expected error name " + expected.name + ", got " + error.name + suffix);
    }
    if (expected.message !== undefined && String(error.message) !== String(expected.message)) {
      throw new Test262Error("Expected message " + expected.message + ", got " + error.message + suffix);
    }
    return;
  }
  throw new Test262Error("assert.throws: unsupported expectation" + suffix);
};
assert.compareArray = function (actual, expected, message) {
  var suffix = message === undefined ? "" : ": " + message;
  if (actual === null || expected === null || typeof actual !== "object" || typeof expected !== "object") {
    throw new Test262Error("compareArray needs two arrays" + suffix);
  }
  if (actual.length === undefined || expected.length === undefined) {
    throw new Test262Error("compareArray needs array-likes" + suffix);
  }
  if (actual.length !== expected.length) {
    throw new Test262Error("compareArray: lengths differ: " + actual.length + " vs " + expected.length + suffix);
  }
  var i = 0;
  while (i < actual.length) {
    if (!$sameValue(actual[i], expected[i])) {
      throw new Test262Error("compareArray: differs at " + i + suffix);
    }
    i = i + 1;
  }
};
assert.defineProperty = function (obj, name, desc) {
  Object.defineProperty(obj, name, desc);
};
assert.objKeys = function (value) {
  var out = [];
  var keys = Object.keys(value);
  var i = 0;
  while (i < keys.length) { out.push(keys[i]); i = i + 1; }
  return out;
};

// propertyHelper.js: the descriptor check almost every property test relies on.
var verifyProperty = function (obj, name, desc, options) {
  var strict = options === undefined || options.restore === undefined || options.restore !== false;
  var actual = Object.getOwnPropertyDescriptor(obj, name);
  if (actual === undefined) {
    throw new Test262Error("Expected an own property descriptor for " + String(name));
  }
  if (desc.value !== undefined && !$sameValue(actual.value, desc.value)) {
    throw new Test262Error("verifyProperty: wrong value for " + String(name) +
                           ": " + actual.value + " vs " + desc.value);
  }
  if (desc.writable !== undefined && actual.writable !== desc.writable) {
    throw new Test262Error("verifyProperty: wrong writable for " + String(name));
  }
  if (desc.enumerable !== undefined && actual.enumerable !== desc.enumerable) {
    throw new Test262Error("verifyProperty: wrong enumerable for " + String(name));
  }
  if (desc.configurable !== undefined && actual.configurable !== desc.configurable) {
    throw new Test262Error("verifyProperty: wrong configurable for " + String(name));
  }
  if (desc.get !== undefined && actual.get !== desc.get) {
    throw new Test262Error("verifyProperty: wrong getter for " + String(name));
  }
  if (desc.set !== undefined && actual.set !== desc.set) {
    throw new Test262Error("verifyProperty: wrong setter for " + String(name));
  }
  return actual;
};
var comparePropertyDescriptor = function (a, b) {
  if (a.value !== b.value) return false;
  if (a.writable !== b.writable) return false;
  if (a.enumerable !== b.enumerable) return false;
  if (a.configurable !== b.configurable) return false;
  return true;
};
var isConstructor = function (value) {
  if (typeof value !== "function") return false;
  try {
    // A proxy-free approximation: a constructor has a prototype object.
    return value.prototype !== undefined;
  } catch (e) {
    return false;
  }
};
)JS";

struct SandboxOutcome {
  bool ran = false;      // false when the child died or timed out
  bool ok = false;
  std::string message;
};

[[nodiscard]] SandboxOutcome evaluate_source(const std::string& path, const std::string& source) {
  SandboxOutcome outcome;

  auto evaluate_here = [&path, &source](js::ExecutionResult& result, std::string& preamble_message) {
    js::Context context;
    // The preamble is evaluated separately so a broken shim never masks a test bug.
    const js::ExecutionResult preamble = context.evaluate(kPreamble, "<harness>");
    if (!preamble.ok) preamble_message = preamble.message;
    result = context.evaluate(source, path);
  };

#if BONK_CONFORMANCE_FORK
  // Every file runs in a memory-capped child: a test that asks for a 2^31-element
  // array (or crashes the interpreter) costs us one case, not the whole run.
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    js::ExecutionResult result;
    std::string preamble_message;
    evaluate_here(result, preamble_message);
    outcome.ran = true;
    outcome.ok = result.ok;
    outcome.message = result.message;
    return outcome;
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fds[0]);
    // The child's own output is noise (uncaught errors from tests); only the pipe
    // carries information back to us.
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      (void)::dup2(devnull, 1);
      (void)::dup2(devnull, 2);
      if (devnull > 2) ::close(devnull);
    }
    // A 512 MB address-space cap is what turns "runaway array" into a failed case.
    // Sanitizers reserve huge virtual ranges, so the cap is skipped under ASan.
#if defined(__SANITIZE_ADDRESS__)
    constexpr bool kCapMemory = false;
#else
    constexpr bool kCapMemory = true;
#endif
    if (kCapMemory) {
      struct rlimit limit{};
      limit.rlim_cur = 512ull * 1024 * 1024;
      limit.rlim_max = 512ull * 1024 * 1024;
      (void)::setrlimit(RLIMIT_AS, &limit);
    }
    ::alarm(10);
    js::ExecutionResult result;
    std::string message;
    bool ok = false;
    try {
      std::string preamble_message;
      evaluate_here(result, preamble_message);
      ok = result.ok;
      message = result.ok ? std::string{} : (result.message.empty() ? std::string{"evaluation failed"} : result.message);
    } catch (const std::exception& error) {
      ok = false;
      message = std::string{"engine threw: "} + error.what();
    } catch (...) {
      ok = false;
      message = "engine threw an unknown exception";
    }
    std::string payload;
    payload.push_back(ok ? '1' : '0');
    payload += message;
    const char* bytes = payload.data();
    std::size_t left = payload.size();
    while (left > 0) {
      const ssize_t written = ::write(fds[1], bytes, left);
      if (written <= 0) break;
      bytes += written;
      left -= static_cast<std::size_t>(written);
    }
    ::close(fds[1]);
    ::_exit(0);
  }
  ::close(fds[1]);
  std::string payload;
  char buffer[512];
  for (;;) {
    const ssize_t got = ::read(fds[0], buffer, sizeof buffer);
    if (got <= 0) break;
    payload.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fds[0]);
  int status = 0;
  (void)::waitpid(pid, &status, 0);
  if (payload.empty() || !WIFEXITED(status)) {
    outcome.ran = false;
    outcome.message = WIFSIGNALED(status) ? "child killed by signal " + std::to_string(WTERMSIG(status))
                                          : "child produced no result (memory cap or timeout)";
    return outcome;
  }
  outcome.ran = true;
  outcome.ok = payload[0] == '1';
  outcome.message = payload.substr(1);
  return outcome;
#else
  js::ExecutionResult result;
  std::string preamble_message;
  evaluate_here(result, preamble_message);
  outcome.ran = true;
  outcome.ok = result.ok;
  outcome.message = result.message;
  return outcome;
#endif
}

[[nodiscard]] bool run_source(const std::string& path, const std::string& source, const Metadata& metadata,
                              std::string& detail) {
  const SandboxOutcome sandbox = evaluate_source(path, source);
  struct ResultView {
    bool ok;
    std::string message;
  } result{sandbox.ok, sandbox.message};
  if (!sandbox.ran) {
    detail = sandbox.message;
    return false;
  }
  if (metadata.negative) {
    if (result.ok) {
      detail = "expected " + (metadata.negative_phase.empty() ? "a failure" : metadata.negative_phase + " error") +
               " (" + metadata.negative_type + "), but the test completed";
      return false;
    }
    if (metadata.negative_phase == "parse" && result.message.find("js.syntax") == std::string::npos) {
      detail = "expected a parse error, got a runtime error: " + result.message;
      return false;
    }
    return true;
  }
  if (!result.ok) {
    detail = result.message.empty() ? "evaluation failed" : result.message;
    return false;
  }
  return true;
}

}  // namespace

SuiteReport run_test262_corpus(std::string_view directory, const SuiteOptions& options) {
  SuiteReport report;
  report.suite = "test262";
  report.source = std::string(directory);
  if (!detail::directory_exists(directory)) return report;

  const std::vector<std::string> files = detail::list_files(directory, ".js", true);
  std::size_t skipped_flags = 0;
  std::size_t skipped_features = 0;
  std::size_t skipped_includes = 0;
  std::size_t considered = 0;
  std::size_t skipped_outside_envelope = 0;

  for (const std::string& path : files) {
    const std::string relative = std::filesystem::path(path)
                                     .lexically_relative(std::filesystem::path(std::string(directory)))
                                     .string();
    if (!in_envelope(relative)) {
      ++skipped_outside_envelope;
      continue;
    }
    const std::string name = std::filesystem::path(path).filename().string();
    if (name.size() >= 12 && name.compare(name.size() - 12, 12, "_FIXTURE.js") == 0) continue;
    if (path.find("/harness/") != std::string::npos || path.find("/intl402/") != std::string::npos) continue;

    const std::string source = detail::read_file(path);
    if (source.empty()) continue;
    const Metadata metadata = parse_metadata(source);

    bool skip = false;
    if (contains(metadata.flags, "module") || contains(metadata.flags, "async") ||
        contains(metadata.flags, "raw") || contains(metadata.flags, "CanBlockIsTrue") ||
        contains(metadata.flags, "CanBlockIsFalse")) {
      skip = true;
      ++skipped_flags;
    }
    if (!skip) {
      for (const std::string& feature : metadata.features) {
        if (unsupported_features().count(feature) != 0) {
          skip = true;
          ++skipped_features;
          break;
        }
      }
    }
    if (!skip) {
      for (const std::string& include : metadata.includes) {
        if (supported_includes().count(include) != 0) continue;
        // A harness file we do not model stays out of the runnable set: guessing at its
        // assertions would turn "not measured" into a fake failure (or a fake pass).
        skip = true;
        ++skipped_includes;
        break;
      }
    }
    if (skip) continue;

    ++considered;
    if (options.limit != 0 && report.total >= options.limit) {
      report.notes.push_back("considered " + std::to_string(considered) + " runnable files out of " +
                             std::to_string(files.size()));
      report.notes.push_back("skipped (flags): " + std::to_string(skipped_flags));
      report.notes.push_back("skipped (unsupported feature): " + std::to_string(skipped_features));
      report.notes.push_back("skipped (harness include): " + std::to_string(skipped_includes));
      report.notes.push_back("skipped (outside the engine envelope): " + std::to_string(skipped_outside_envelope));
      return report;
    }
    ++report.total;

    std::string failure_detail;
    if (run_source(path, source, metadata, failure_detail)) {
      ++report.passed;
    } else if (report.failures.size() < options.max_failures) {
      report.failures.push_back({relative, failure_detail});
    }
  }

  report.notes.push_back("considered " + std::to_string(considered) + " runnable files out of " +
                         std::to_string(files.size()));
  report.notes.push_back("skipped (flags): " + std::to_string(skipped_flags));
  report.notes.push_back("skipped (unsupported feature): " + std::to_string(skipped_features));
  report.notes.push_back("skipped (harness include): " + std::to_string(skipped_includes));
  report.notes.push_back("skipped (outside the engine envelope): " + std::to_string(skipped_outside_envelope));
  report.excluded = skipped_flags + skipped_features + skipped_includes + skipped_outside_envelope;
  return report;
}

}  // namespace bonk::conformance
