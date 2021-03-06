/**
 * Copyright : falcon build system (c) 2014.
 * LICENSE : see accompanying LICENSE file for details.
 */

#include <iostream>
#include <cstdlib>

#include "cache_manager.h"
#include "build_plan.h"
#include "daemon_instance.h"
#include "exceptions.h"
#include "fs.h"
#include "graph.h"
#include "graph_consistency_checker.h"
#include "graph_dependency_scan.h"
#include "graph_parallel_builder.h"
#include "logging.h"
#include "options.h"
#include "stream_consumer.h"

static void setOptions(falcon::Options& opt) {
  /* get the default working directory path from then environment variable */
  char const* pwd = NULL;
  pwd = getenv("PWD"); /* No need to free this string */
  if (!pwd) {
    LOG(ERROR) << "enable to get the PWD";
    return;
  }
  std::string pwdString(pwd);

  /* *********************************************************************** */
  /* add command line options */
  opt.addCLIOption("daemon,d", "daemonize the build system");
  opt.addCLIOption("module,M",
                   po::value<std::string>(),
                   "use -M help for more info");
  opt.addCLIOption("config,f", /* TODO */
                   po::value<std::string>(),
                   "falcon configuration file");

  /* *********************************************************************** */
  /* add command line option and configuration file option (this options can be
   * setted on both configuration file or from the CLI) */
  opt.addCFileOption("working-directory",
                     po::value<std::string>()->default_value(pwdString),
                     "falcon working directory path");
  opt.addCFileOption("graph",
                     po::value<std::string>()->default_value("makefile.json"),
                     "falcon graph file");
  opt.addCFileOption("api-port",
                     po::value<int>()->default_value(4242),
                     "the API listening port");
  opt.addCFileOption("stream-port",
                     po::value<int>()->default_value(4343),
                     "stream port");
  opt.addCFileOption("log-level",
                     po::value<google::LogSeverity>()->default_value(google::GLOG_WARNING),
                     "define the log level");
  opt.addCFileOption("log-dir",
                     po::value<std::string>(),
                     "write log files in the given directory");
}

static int loadModule(falcon::Graph& g, std::string const& s) {

  LOG(INFO) << "load module '" << s << "'";

  if (0 == s.compare("dot")) {
    printGraphGraphviz(g, std::cout);
  } else if (0 == s.compare("make")) {
    printGraphMakefile(g, std::cout);
  } else if (0 == s.compare("help")) {
    std::cout << "list of available modules: " << std::endl
      << "  dot    show the graph in DOT format" << std::endl
      << "  make   show the graph in Makefile format" << std::endl;
  } else {
    LOG(ERROR) << "module '" << s << "' not supported";
    return 1;
  }

  return 0;
}

/**
 * Daemonize the current process.
 */
static void daemonize(std::unique_ptr<falcon::GlobalConfig> config,
                      std::unique_ptr<falcon::Graph> graph,
                      std::unique_ptr<falcon::CacheManager> cache) {

  /** the double-fork-and-setsid trick establishes a child process that runs in
   * its own process group with its own session and that won't get killed off
   * when your shell exits (for example). */
  if (fork()) { return; }
  setsid();
  if (fork()) { return; }

  falcon::DaemonInstance daemon(std::move(config), std::move(cache));
  daemon.loadConf(std::move(graph));
  daemon.start();
}

int main (int const argc, char const* const* argv) {
  falcon::Options opt;

  setOptions(opt);

  /* parse the command line options */
  try {
    opt.parseOptions(argc, argv);
  } catch (falcon::Exception& e) {
    if (e.getCode () != 0) { // the help throw a SUCCESS code, no error to show
      std::cerr << e.getErrorMessage() << std::endl;
    }
    return e.getCode();
  }

  std::unique_ptr<falcon::GlobalConfig> config(new falcon::GlobalConfig(opt));

  falcon::fs::mkdir(config->getFalconDir());

  /* Analyze the graph given in the configuration file */
  falcon::GraphParser graphParser(config->getJsonGraphFile());
  try {
    graphParser.processFile();
  } catch (falcon::Exception& e) {
    LOG(ERROR) << e.getErrorMessage ();
    return e.getCode();
  }

  std::unique_ptr<falcon::Graph> graphPtr = std::move(graphParser.getGraph());

  /* Check the graph for cycles. */
  try {
    checkGraphLoop(*graphPtr);
  } catch (falcon::Exception& e) {
    LOG(ERROR) << e.getErrorMessage();
    return e.getCode();
  }

  std::unique_ptr<falcon::CacheManager> cache(
      new falcon::CacheManager(config->getWorkingDirectoryPath(),
                               config->getFalconDir()));

  /* Scan the graph to discover what needs to be rebuilt, and compute the
   * hashes of all nodes. */
  falcon::GraphDependencyScan scanner(*graphPtr, cache.get());
  scanner.scan();

  /* if a module has been requested to execute then load it and return */
  if (opt.isOptionSetted("module")) {
    return loadModule(*graphPtr,
                      opt.vm_["module"].as<std::string>());
  }

  /* Start the daemon. */
  daemonize(std::move(config), std::move(graphPtr), std::move(cache));
  return 0;
}
