/**
 * Copyright : falcon build system (c) 2014.
 * LICENSE : see accompanying LICENSE file for details.
 */

#ifndef FALCON_GRAPH_SEQUENTIAL_BUILDER_H_
#define FALCON_GRAPH_SEQUENTIAL_BUILDER_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "graph.h"
#include "posix_subprocess.h"

namespace falcon {

class WatchmanClient;
class StreamConsumer;

enum class BuildResult { UNKNOWN, SUCCEEDED, INTERRUPTED, FAILED };
std::string toString(BuildResult v);

typedef std::function<void(BuildResult)> onBuildCompletedFn;

class IGraphBuilder {
 public:
  virtual ~IGraphBuilder() {}

  /**
   * Launch an asynchronous build of the given targets.
   * @param targets Targets to build.
   * @param cb Callback to be called when the build completes.
   */
  virtual void startBuild(NodeSet& targets, onBuildCompletedFn cb) = 0;

  /**
   * Interrupt the build. The callback will be called with the INTERRUPTED
   * code.
   */
  virtual void interrupt() = 0;

  /**
   * Wait until the current build completes.
   */
  virtual void wait() = 0;

  virtual BuildResult getResult() const = 0;
};

class GraphSequentialBuilder : public IGraphBuilder {
 public:
  GraphSequentialBuilder(Graph& graph, std::mutex& mutex,
                         WatchmanClient* watchmanClient,
                         std::string const& workingDirectory,
                         IStreamConsumer* consumer);
  ~GraphSequentialBuilder();

  void startBuild(NodeSet& targets, onBuildCompletedFn cb);

  void interrupt();

  void wait();

  BuildResult getResult() const { return res_; }

 private:
  /* Entry point of the thread that performs the build. */
  void buildThread(NodeSet& targets);

  /**
   * Build the given target.
   * @param target Target to build.
   * @return BuildResult::SUCCEEDED on success,
   *         BuildResult::INTERRUPTED if the user interrupted the build,
   *         BuildResult::FAILED if one of the sub commands failed.
   */
  BuildResult buildTarget(Node* target);

  PosixSubProcessManager manager_;
  WatchmanClient* watchmanClient_;
  Graph& graph_;
  std::unique_lock<std::mutex> lock_;
  std::string workingDirectory_;
  std::atomic_bool interrupted_;
  /* use to keep the depth when building */
  unsigned depth_;
  BuildResult res_;

  onBuildCompletedFn callback_;
  std::thread thread_;
};

} // namespace falcon

#endif // FALCON_GRAPH_SEQUENTIAL_BUILDER_H_

