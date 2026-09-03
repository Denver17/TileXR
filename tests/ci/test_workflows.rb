#!/usr/bin/env ruby

require "yaml"

unless YAML.respond_to?(:safe_load_file)
  def YAML.safe_load_file(path, aliases: false)
    safe_load(File.read(path), aliases: aliases, filename: path)
  end
end

ROOT = File.expand_path("../..", __dir__)
PR_WORKFLOW_PATH = File.join(ROOT, ".github/workflows/pr-ci.yml")
NPU_WORKFLOW_PATH = File.join(ROOT, ".github/workflows/npu-ci.yml")
CODEOWNERS_PATH = File.join(ROOT, ".github/CODEOWNERS")
CHECKOUT_ACTION = "actions/checkout@9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0"
UPLOAD_ACTION = "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a"

def assert(condition, message)
  raise message unless condition
end

def assert_equal(expected, actual, message)
  return if expected == actual

  raise "#{message}: expected #{expected.inspect}, got #{actual.inspect}"
end

def load_workflow(path)
  YAML.safe_load_file(path, aliases: true)
end

def steps_using(workflow, action)
  workflow.fetch("jobs").values.flat_map { |job| job.fetch("steps", []) }
          .select { |step| step["uses"] == action }
end

def named_step(workflow, job_name, step_name)
  workflow.fetch("jobs").fetch(job_name).fetch("steps")
          .find { |step| step["name"] == step_name }
end

pr = load_workflow(PR_WORKFLOW_PATH)
npu = load_workflow(NPU_WORKFLOW_PATH)

assert_equal(["pull_request"], pr.fetch("on").keys,
             "PR workflow must use only pull_request")
pull_request = pr.fetch("on").fetch("pull_request")
assert_equal(["main"], pull_request.fetch("branches"),
             "PR workflow must target main")
assert_equal(%w[opened reopened synchronize ready_for_review converted_to_draft closed],
             pull_request.fetch("types"), "PR event types differ")
assert_equal({"contents" => "read"}, pr.fetch("permissions"),
             "PR workflow permissions differ")

concurrency = pr.fetch("concurrency")
assert_equal("tilexr-pr-${{ github.event.pull_request.number }}",
             concurrency.fetch("group"), "PR concurrency group differs")
assert_equal(true, concurrency.fetch("cancel-in-progress"),
             "obsolete PR runs must be cancelled")

npu_caller = pr.fetch("jobs").fetch("npu_gate")
assert(npu_caller.fetch("if").include?("github.event.action != 'closed'"),
       "closed pull requests must not start an NPU gate")
assert_equal("LingquLab/TileXR/.github/workflows/npu-ci.yml@main",
             npu_caller.fetch("uses"), "NPU reusable workflow identity differs")
assert_equal({"contents" => "read"}, npu_caller.fetch("permissions"),
             "NPU caller permissions differ")
assert(pr.fetch("jobs").fetch("host_checks").fetch("if").include?(
         "github.event.action != 'closed'"),
       "closed pull requests must not run host checks")
assert(pr.fetch("jobs").fetch("pr_gate").fetch("if").include?(
         "github.event.action != 'closed'"),
       "closed pull requests must only cancel the obsolete run")
safe_directory = named_step(pr, "host_checks", "Trust container checkout")
assert_equal('git config --global --add safe.directory "$GITHUB_WORKSPACE"',
             safe_directory.fetch("run"),
             "container checkout must be trusted before Git-based host checks")

assert_equal(["workflow_call"], npu.fetch("on").keys,
             "NPU workflow must use only workflow_call")
assert_equal({"contents" => "read"}, npu.fetch("permissions"),
             "NPU workflow permissions differ")
hardware = npu.fetch("jobs").fetch("hardware")
assert_equal(660, hardware.fetch("timeout-minutes"),
             "NPU timeout differs")
assert_equal("TileXR-NPU", hardware.fetch("runs-on").fetch("group"),
             "NPU runner group differs")
assert_equal(%w[self-hosted Linux ARM64 tilexr ascend910b npu8],
             hardware.fetch("runs-on").fetch("labels"),
             "NPU runner labels differ")

checkout_steps = [pr, npu].flat_map { |workflow| steps_using(workflow, CHECKOUT_ACTION) }
assert_equal(2, checkout_steps.length, "each workflow must use pinned checkout")
checkout_steps.each do |step|
  assert_equal(false, step.fetch("with").fetch("persist-credentials"),
               "checkout must disable credential persistence")
end
npu_checkout = steps_using(npu, CHECKOUT_ACTION).fetch(0)
assert_equal("${{ github.sha }}",
             npu_checkout.fetch("with").fetch("ref"),
             "NPU checkout must pin the workflow merge SHA")

assert_equal([], [pr, npu].flat_map { |workflow| steps_using(workflow, UPLOAD_ACTION) },
             "CI workflows must not upload artifacts")

identity_keys = %w[PR_NUMBER HEAD_SHA BASE_SHA EXPECTED_MERGE_SHA]
host_env = named_step(pr, "host_checks", "Run host checks").fetch("env")
assert_equal(identity_keys.map { |key| "TILEXR_CI_#{key}" }.sort,
             host_env.keys.sort, "host summary identity inputs differ")
npu_env = named_step(npu, "hardware", "Run trusted gate").fetch("env")
identity_keys.each do |key|
  assert(npu_env.key?("TILEXR_CI_#{key}"),
         "NPU summary is missing #{key} identity input")
end
assert(!npu_env.key?("GITHUB_TOKEN"),
       "NPU controller must not receive a GitHub token")
assert(!npu_env.key?("TILEXR_CI_GITHUB_TOKEN"),
       "legacy controller token environment is forbidden")

gate_run = named_step(npu, "hardware", "Run trusted gate").fetch("run")
assert(gate_run.include?("exec python3 /home/tilexr-ci/control/current/gate.py"),
       "NPU workflow must exec the active sealed controller")
assert(!File.read(NPU_WORKFLOW_PATH).include?("/control/current/VERSION"),
       "NPU workflow must not pin the active controller version")
assert(!gate_run.include?("GITHUB_TOKEN"),
       "controller step must not reference a GitHub token")
assert(!gate_run.include?("github-token"),
       "controller command must not accept a GitHub token")
assert(!gate_run.include?("&"), "NPU gate must not run in the background")
assert(!gate_run.match?(/(?:^|\n)\s*wait\b/),
       "NPU gate must not leave a waiting parent shell")
assert_equal('--pr-number "${TILEXR_CI_PR_NUMBER}"', gate_run.lines.last.strip,
             "controller exec must be the final shell command")

summary = named_step(pr, "pr_gate", "Summarize gate")
assert_equal(%w[PR_NUMBER HEAD_SHA BASE_SHA MERGE_SHA],
             summary.fetch("env").keys.first(4),
             "final gate summary identity inputs differ")
%w[PR Head\ SHA Base\ SHA Merge\ SHA].each do |label|
  assert(summary.fetch("run").include?("- #{label}:"),
         "final gate summary is missing #{label}")
end

workflow_text = [PR_WORKFLOW_PATH, NPU_WORKFLOW_PATH].map { |path| File.read(path) }.join("\n")
%w[pull_request_target secrets:\ inherit].each do |forbidden|
  assert(!workflow_text.include?(forbidden),
         "workflow contains forbidden text #{forbidden.inspect}")
end
[pr, npu].each do |workflow|
  workflow.fetch("jobs").each_value do |job|
    permissions = job.fetch("permissions", {})
    assert(!permissions.value?("write"), "workflow job grants write permission")
  end
  assert(!workflow.fetch("permissions", {}).value?("write"),
         "workflow grants write permission")
end

expected_owners = {
  "/.github/CODEOWNERS" => "@LingquLab/ci-maintainers",
  "/.github/workflows/" => "@LingquLab/ci-maintainers",
  "/.github/actions/" => "@LingquLab/ci-maintainers",
  "/scripts/ci/control/" => "@LingquLab/ci-maintainers",
  "/scripts/ci/provision/" => "@LingquLab/ci-maintainers",
  "/scripts/cann_download_install.sh" => "@LingquLab/ci-maintainers",
  "/scripts/cann_local_install.sh" => "@LingquLab/ci-maintainers",
  "/scripts/common_env.sh" => "@LingquLab/ci-maintainers",
  "/scripts/common_util.sh" => "@LingquLab/ci-maintainers",
  "/.gitmodules" => "@LingquLab/ci-maintainers",
}
actual_owners = File.readlines(CODEOWNERS_PATH, chomp: true).reject(&:empty?).to_h do |line|
  line.split(/\s+/, 2)
end
assert_equal(expected_owners, actual_owners, "CODEOWNERS CI boundary differs")

puts "workflow policy validation passed"
