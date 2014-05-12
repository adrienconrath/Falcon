/**
 * Copyright : falcon build system (c) 2014.
 * LICENSE : see accompanying LICENSE file for details.
 */

#ifndef FALCON_GRAPH_RELOADER_H_
# define FALCON_GRAPH_RELOADER_H_

# include "graph.h"
# include "watchman.h"

namespace falcon {

class CacheManager;

class GraphReloader {
public:
  GraphReloader(Graph* original, Graph* newGraph, WatchmanClient& watchman,
                CacheManager* cache);
  ~GraphReloader();

  Graph* getUpdatedGraph();

private:
  Graph* original_;
  Graph* new_;
  WatchmanClient& watchman_;
  CacheManager* cache_;
};

}

#endif /* !FALCON_GRAPH_RELOADER_H_ */