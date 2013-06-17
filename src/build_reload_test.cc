// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "build.h"

#include "build_log.h"
#include "deps_log.h"
#include "graph.h"
#include "test.h"

/// Fake implementation of CommandRunner, useful for tests.
struct YAFakeCommandRunner : public CommandRunner {
  explicit YAFakeCommandRunner(VirtualFileSystem* fs) :
      last_command_(NULL), fs_(fs) {}

  // CommandRunner impl
  virtual bool CanRunMore();
  virtual bool StartCommand(Edge* edge);
  virtual bool WaitForCommand(Result* result);
  virtual vector<Edge*> GetActiveEdges();
  virtual void Abort();

  bool MkDepCmd(const Edge* edge);

  vector<string> commands_ran_;
  Edge* last_command_;
  VirtualFileSystem* fs_;
};

struct YABuildTest : public StateTestWithBuiltinRules {
  YABuildTest() : config_(MakeConfig()), command_runner_(&fs_),
                builder_(&state_, config_, NULL, NULL, &fs_),
                status_(config_) {
  }

  virtual void SetUp() {
    StateTestWithBuiltinRules::SetUp();

    builder_.command_runner_.reset(&command_runner_);
    AssertParse(&state_,
"build cat1: cat in1\n"
"build cat2: cat in1 in2\n"
"build cat12: cat cat1 cat2\n");

    fs_.Create("in1", "");
    fs_.Create("in2", "");
  }

  ~YABuildTest() {
    builder_.command_runner_.release();
  }

  // Mark a path dirty.
  void Dirty(const string& path);

  BuildConfig MakeConfig() {
    BuildConfig config;
    config.verbosity = BuildConfig::QUIET;
    return config;
  }

  // Useful for debugging
  void DumpCommandsRan() const;

  BuildConfig config_;
  YAFakeCommandRunner command_runner_;
  VirtualFileSystem fs_;
  Builder builder_;

  BuildStatus status_;
};

bool YAFakeCommandRunner::CanRunMore() {
  // Only run one at a time.
  return last_command_ == NULL;
}

bool YAFakeCommandRunner::MkDepCmd(const Edge* edge)
{
  string err;
  const string& out_path = edge->outputs_[0]->path();
  string in_path = edge->inputs_[0]->path();
  string deps = out_path + ": " + in_path;

  // Read input file. If its content starts with '%' then
  //  treat the following symbols as an included file path.
  //  Update 'depfile' and repeat with the 'included' path.
  for(;;) {
    string content = fs_->ReadFile(in_path, &err);
    if (!err.empty())
      return false;
    if (content.find('%') != 0)
      break;
    in_path = content.substr(1);
    deps += " " + in_path;
  }
  fs_->Create(out_path, deps);
  return true;
}

bool YAFakeCommandRunner::StartCommand(Edge* edge) {
  assert(!last_command_);
  fs_->Tick();
  commands_ran_.push_back(edge->EvaluateCommand());
  if (edge->rule().name() == "cat"  ||
      edge->rule().name() == "cat_rsp" ||
      edge->rule().name() == "cc" ||
      edge->rule().name() == "touch" ||
      edge->rule().name() == "touch-interrupt") {
    for (vector<Node*>::iterator out = edge->outputs_.begin();
         out != edge->outputs_.end(); ++out) {
      fs_->Create((*out)->path(), edge->env_->LookupVariable("content"));
    }
  } else if (edge->rule().name() == "mkdep") {
    if (!MkDepCmd(edge))
      return false;
  } else if (edge->rule().name() == "true" ||
             edge->rule().name() == "fail" ||
             edge->rule().name() == "interrupt") {
    // Don't do anything.
  } else {
    printf("unknown command. rule: %s\n", edge->rule().name().c_str());
    return false;
  }

  last_command_ = edge;
  return true;
}

bool YAFakeCommandRunner::WaitForCommand(Result* result) {
  if (!last_command_)
    return false;

  Edge* edge = last_command_;
  result->edge = edge;

  if (edge->rule().name() == "interrupt" ||
      edge->rule().name() == "touch-interrupt") {
    result->status = ExitInterrupted;
    return true;
  }

  if (edge->rule().name() == "fail")
    result->status = ExitFailure;
  else
    result->status = ExitSuccess;
  last_command_ = NULL;
  return true;
}

vector<Edge*> YAFakeCommandRunner::GetActiveEdges() {
  vector<Edge*> edges;
  if (last_command_)
    edges.push_back(last_command_);
  return edges;
}

void YAFakeCommandRunner::Abort() {
  last_command_ = NULL;
}

void YABuildTest::DumpCommandsRan() const {
  for (vector<string>::const_iterator i = command_runner_.commands_ran_.begin();
       i != command_runner_.commands_ran_.end(); ++i)
    printf("Executed: %s\n", (*i).c_str());
}

void YABuildTest::Dirty(const string& path) {
  Node* node = GetNode(path);
  node->MarkDirty();

  // If it's an input file, mark that we've already stat()ed it and
  // it's missing.
  if (!node->in_edge())
    node->MarkMissing();
}

TEST_F(YABuildTest, DepFileReloadNested) {
  string err;
  state_ = State(); //blank state
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule mkdep\n"
"  command = MkDepCmd -o $out $in\n"
"  depfile = $out\n"
"  reload = 1\n"
"rule cat\n"
"  command = echo $content > $out\n"
"rule cc\n"
"  command = cc -o $out $in\n"
""
"build config.h: cat\n"
"  content = #define x\n"
"build auto.h: cat\n"
"  content = %config.h\n"
"build auto.c: cat\n"
"  content = %auto.h\n"
"build auto.o.dd: mkdep auto.c\n"
"build auto.o: cc auto.c | auto.o.dd\n"));

  EXPECT_TRUE(builder_.AddTarget("auto.o", &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(3, builder_.plan_.command_edge_count()); // auto.{o,c,o.dd}
  ASSERT_EQ(5, state_.edges_.size());

  EXPECT_TRUE(builder_.Build(&err));
  EXPECT_EQ("", err);

  // (Only this build order is valid)
  vector<string>& commands_ran_ = command_runner_.commands_ran_;
  ASSERT_EQ(7u, commands_ran_.size()); // 5 edges + 'auto.o.dd' restarted twice
  ASSERT_EQ(commands_ran_[0], "echo %auto.h > auto.c");
  ASSERT_EQ(commands_ran_[1], "MkDepCmd -o auto.o.dd auto.c");
  ASSERT_EQ(commands_ran_[2], "echo %config.h > auto.h");
  ASSERT_EQ(commands_ran_[3], "MkDepCmd -o auto.o.dd auto.c");
  ASSERT_EQ(commands_ran_[4], "echo #define x > config.h");
  ASSERT_EQ(commands_ran_[5], "MkDepCmd -o auto.o.dd auto.c");
  ASSERT_EQ(commands_ran_[6], "cc -o auto.o auto.c");

  // Verify that restarted edges are counted properly
  EXPECT_EQ("[7/7/7]", builder_.status_->FormatProgressStatus("[%s/%t/%f]"));

  // Validate that auto.o.dd depends now on auto.h and config.h
  vector<Node*>& ddeps = GetNode("auto.o.dd")->in_edge()->inputs_;
  if (find(ddeps.begin(), ddeps.end(), GetNode("auto.h")) == ddeps.end())
    FAIL();
  if (find(ddeps.begin(), ddeps.end(), GetNode("config.h")) == ddeps.end())
    FAIL();

  // Finally, verify that expected content was built
  EXPECT_EQ("auto.o.dd: auto.c auto.h config.h", fs_.ReadFile("auto.o.dd", &err));
  EXPECT_EQ("%config.h", fs_.ReadFile("auto.h", &err));
  EXPECT_EQ("#define x", fs_.ReadFile("config.h", &err));
}

TEST_F(YABuildTest, DepFileReloadRebuild) {
  string err;
  state_ = State(); //blank state
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule cat\n"
"  command = echo $content > $out\n"
"rule mkdep\n"
"  command = MkDepCmd -o $out $in\n"
"  depfile = $out\n"
"  reload = 1\n"
"rule cc\n"
"  command = cc -o $out $in\n"
""
"build config.h: cat\n"
"  content = #define x\n"
"build auto.h: cat\n"
"  content = %config.h\n"
"build auto.c: cat\n"
"  content = %auto.h\n"
"build auto.o.dd: mkdep auto.c\n"
"build auto.o: cc auto.c | auto.o.dd\n"));

  vector<string>& commands_ran_ = command_runner_.commands_ran_;

  // 1st build - build everything
  EXPECT_TRUE(builder_.AddTarget("auto.o", &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(builder_.Build(&err));
  EXPECT_EQ("", err);
  ASSERT_EQ(7u, commands_ran_.size());

  // Second build is noop
  state_.Reset();
  commands_ran_.clear();
  EXPECT_TRUE(builder_.AddTarget("auto.o", &err));
  EXPECT_TRUE(builder_.AlreadyUpToDate());

  // One of the inputs was updated after dep file creation
  state_.Reset();
  commands_ran_.clear();
  Dirty("auto.h");
  EXPECT_TRUE(builder_.AddTarget("auto.o", &err));
  EXPECT_TRUE(builder_.Build(&err));
  EXPECT_EQ("", err);
  ASSERT_EQ(2u, commands_ran_.size());
  ASSERT_EQ(commands_ran_[0], "MkDepCmd -o auto.o.dd auto.c");
  ASSERT_EQ(commands_ran_[1], "cc -o auto.o auto.c");

  // Second build is noop
  state_.Reset();
  commands_ran_.clear();
  EXPECT_TRUE(builder_.AddTarget("auto.o", &err));
  EXPECT_TRUE(builder_.AlreadyUpToDate());

  // Dep file removed
  state_.Reset();
  commands_ran_.clear();
  EXPECT_TRUE(fs_.RemoveFile("auto.o.dd") == 0);
  EXPECT_TRUE(builder_.AddTarget("auto.o", &err));
  EXPECT_TRUE(builder_.Build(&err));
  EXPECT_EQ("", err);
  ASSERT_EQ(2u, commands_ran_.size());
  ASSERT_EQ(commands_ran_[0], "MkDepCmd -o auto.o.dd auto.c");
  ASSERT_EQ(commands_ran_[1], "cc -o auto.o auto.c");

  // Second build is noop
  state_.Reset();
  commands_ran_.clear();
  EXPECT_TRUE(builder_.AddTarget("auto.o", &err));
  EXPECT_TRUE(builder_.AlreadyUpToDate());
}

TEST_F(YABuildTest, DepFileReloadBadDep) {
  string err;
  state_ = State(); //blank state
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule cat\n"
"  command = echo $content > $out\n"
"  depfile = $out\n"
"  reload = 1\n"
""
"build auto.dd: cat\n"
"  content = clutter\n"));

  EXPECT_TRUE(builder_.AddTarget("auto.dd", &err));
  ASSERT_EQ("", err);
  // Then bad depfile built and reloaded
  EXPECT_FALSE(builder_.Build(&err));
  EXPECT_EQ("expected depfile 'auto.dd' to mention 'auto.dd', got 'clutter'", err);
}

// Reload rule failure
TEST_F(YABuildTest, DepFileReloadRuleFails) {
  string err;
  state_ = State(); //blank state
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule fail\n"
"  command = fail\n"
"  depfile = $out\n"
"  reload = 1\n"
""
"build out1: fail\n"
"  content = clutter\n"));

  EXPECT_TRUE(builder_.AddTarget("out1", &err));
  ASSERT_EQ("", err);
  // Then bad depfile built and reloaded
  EXPECT_FALSE(builder_.Build(&err));
  EXPECT_EQ("subcommand failed", err);
}

// Check that response file is created and deleted for 'reload = 1' targets
TEST_F(YABuildTest, DepFileReloadWithRsp) {
  string err;
  state_ = State(); //blank state
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule cat\n"
"  command = echo $content > $out\n"
"rule mkdep\n"
"  command = MkDepCmd -o $out $in\n"
"  rspfile = $out.rsp\n"
"  rspfile_content = $long_command\n"
"  depfile = $out\n"
"  reload = 1\n"
"rule cc\n"
"  command = cc -o $out $in\n"
""
"build config.h: cat\n"
"  content = #define x\n"
"build auto.h: cat\n"
"  content = %config.h\n"
"build auto.c: cat\n"
"  content = %auto.h\n"
"build auto.o.dd: mkdep auto.c\n"
"  long_command = Long Long Long command\n"
"build auto.o: cc auto.c | auto.o.dd\n"));

  vector<string>& commands_ran_ = command_runner_.commands_ran_;

  // 1st build - build everything
  EXPECT_TRUE(builder_.AddTarget("auto.o", &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(builder_.Build(&err));
  EXPECT_EQ("", err);
  ASSERT_EQ(7u, commands_ran_.size());

  // RSP file(s) were created
  EXPECT_EQ(1, fs_.files_created_.count("auto.o.dd.rsp"));

  // RSP file(s) were removed
  EXPECT_EQ(1, fs_.files_removed_.count("auto.o.dd.rsp"));
  EXPECT_EQ(0, fs_.Stat("auto.o.dd.rsp"));
}

/// Tests of builds involving deps logs necessarily must span
/// multiple builds.  We reuse methods on YABuildTest but not the
/// builder_ it sets up, because we want pristine objects for
/// each build.
struct YABuildWithDepsLogTest : public YABuildTest {
  YABuildWithDepsLogTest() {}

  virtual void SetUp() {
    YABuildTest::SetUp();

    temp_dir_.CreateAndEnter("YABuildWithDepsLogTest");
  }

  virtual void TearDown() {
    temp_dir_.Cleanup();
  }

  ScopedTempDir temp_dir_;

  /// Shadow parent class builder_ so we don't accidentally use it.
  void* builder_;
};

TEST_F(YABuildWithDepsLogTest, DepFileReloadNestedWithDeps) {
  string err;
  const char* manifest = \
"rule mkdep\n"
"  command = MkDepCmd -o $out $in\n"
"  deps = gcc\n"
"  depfile = $out\n"
"  reload = 1\n"
"rule cat\n"
"  command = echo $content > $out\n"
"rule cc\n"
"  command = cc -o $out $in\n"
""
"build config.h: cat\n"
"  content = #define x\n"
"build auto.h: cat\n"
"  content = %config.h\n"
"build auto.c: cat\n"
"  content = %auto.h\n"
"build auto.o.dd: mkdep auto.c\n"
"build auto.o: cc auto.c | auto.o.dd\n";
  {
    // Run the build once, everything should be ok.
    State state;
    ASSERT_NO_FATAL_FAILURE(AssertParse(&state, manifest));

    DepsLog deps_log;
    ASSERT_TRUE(deps_log.OpenForWrite("ninja_deps", &err));
    ASSERT_EQ("", err);

    Builder builder(&state, config_, NULL, &deps_log, &fs_);
    builder.command_runner_.reset(&command_runner_);

    EXPECT_TRUE(builder.AddTarget("auto.o", &err));
    ASSERT_EQ("", err);
    ASSERT_EQ(3, builder.plan_.command_edge_count()); // auto.{o,c,o.dd}
    ASSERT_EQ(5, state.edges_.size());

    EXPECT_TRUE(builder.Build(&err));
    EXPECT_EQ("", err);

    // (Only this build order is valid)
    vector<string>& commands_ran = command_runner_.commands_ran_;
    ASSERT_EQ(7u, commands_ran.size()); // 5 edges + 'auto.o.dd' restarted twice
    ASSERT_EQ(commands_ran[0], "echo %auto.h > auto.c");
    ASSERT_EQ(commands_ran[1], "MkDepCmd -o auto.o.dd auto.c");
    ASSERT_EQ(commands_ran[2], "echo %config.h > auto.h");
    ASSERT_EQ(commands_ran[3], "MkDepCmd -o auto.o.dd auto.c");
    ASSERT_EQ(commands_ran[4], "echo #define x > config.h");
    ASSERT_EQ(commands_ran[5], "MkDepCmd -o auto.o.dd auto.c");
    ASSERT_EQ(commands_ran[6], "cc -o auto.o auto.c");

    // Verify that restarted edges are counted properly
    EXPECT_EQ("[7/7/7]", builder.status_->FormatProgressStatus("[%s/%t/%f]"));

    // Validate that auto.o.dd depends now on auto.h and config.h
    vector<Node*>& ddeps = state.GetNode("auto.o.dd")->in_edge()->inputs_;
    if (find(ddeps.begin(), ddeps.end(), state.GetNode("auto.h")) == ddeps.end())
      FAIL();
    if (find(ddeps.begin(), ddeps.end(), state.GetNode("config.h")) == ddeps.end())
      FAIL();

    // Verify that expected content was built and dep file was erased
    EXPECT_EQ("", fs_.ReadFile("auto.o.dd", &err));
    EXPECT_EQ("%config.h", fs_.ReadFile("auto.h", &err));
    EXPECT_EQ("#define x", fs_.ReadFile("config.h", &err));

    deps_log.Close();
    builder.command_runner_.release();
  }
  {
    State state;
    ASSERT_NO_FATAL_FAILURE(AssertParse(&state, manifest));

    DepsLog deps_log;
    ASSERT_TRUE(deps_log.Load("ninja_deps", &state, &err));
    ASSERT_TRUE(deps_log.OpenForWrite("ninja_deps", &err));
    ASSERT_EQ("", err);

    Builder builder(&state, config_, NULL, &deps_log, &fs_);
    builder.command_runner_.reset(&command_runner_);

    // Second build is noop
    vector<string>& commands_ran = command_runner_.commands_ran_;
    commands_ran.clear();
    EXPECT_TRUE(builder.AddTarget("auto.o", &err));
    EXPECT_TRUE(builder.AlreadyUpToDate());

    deps_log.Close();
    builder.command_runner_.release();
  }
}
