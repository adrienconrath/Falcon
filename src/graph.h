/**
 * Copyright : falcon build system (c) 2014.
 * LICENSE : see accompanying LICENSE file for details.
 */

#ifndef FALCON_GRAPH_H_
# define FALCON_GRAPH_H_

# include <set>
# include <string>
# include <vector>
# include <unordered_map>

/** This file defines the data structure for storing the Graph of Nodes and
 * Rules.
 *
 * - Node: this is either a source file or a target. A Node is generated by only
 *   one Rule (it's child), but can be the input of many rules (it's parents).
 *
 * - Rule: it takes several nodes as input and generates several output nodes.
 *
 * - Graph: this is the data structure that stores the graph of Nodes and
 *   Rules. It stores a vector of root nodes, ie the nodes that do not generate
 *   any other node, ie the nodes that have no parent.
 */

namespace falcon {

class Node;
class Rule;
class GraphVisitor;

typedef std::vector<Node*>                     NodeArray;
typedef std::set<Node*>                        NodeSet;
typedef std::unordered_map<std::string, Node*> NodeMap;
typedef std::vector<Rule*>                     RuleArray;
typedef std::set<Rule*>                        RuleSet;
typedef unsigned int                           TimeStamp;

/** Define the state of a node or rule. */
enum class State { UP_TO_DATE, OUT_OF_DATE };

/** Class that represents a node in the graph. */
class Node {
 public:
  explicit Node(const std::string& path);

  const std::string& getPath() const;

  /**
   * Set a rule to be the child of this node, ie this node is generated by it.
   */
  void setChild(Rule* rule);
  Rule* getChild();
  const Rule* getChild() const;

  /**
   * Add a rule to be a parent rule of this Node, ie this node is an input of
   * the rule.
   * @param rule Rule to be set as a parent rule.
   */
  void addParentRule(Rule* rule);
  RuleArray& getParents();
  const RuleArray& getParents() const;

  /* State management */
  State const& getState() const;
  State& getState();
  bool isDirty() const;
  void setState(State state);
  /* Set the state as Dirty and mark all the dependencies as dirty too */
  void markDirty();
  /* Set the state as Up to date and mark all the dependencies */
  void markUpToDate();

  TimeStamp getTimeStamp() const;
  TimeStamp getPreviousTimeStamp () const;
  void updateTimeStamp(TimeStamp const);

  /* Operators */
  bool operator==(Node const& n) const;
  bool operator!=(Node const& n) const;

  void accept(GraphVisitor& v);

 private:
  std::string path_;

  /* The rule used to construct this Node.
   * If nullptr, this node is a source file (a leaf node). */
  Rule* childRule_;

  /* The rules that take this node as an input.
  * If empty, this node is a root node because it does not generate any other
  * node. */
  RuleArray parentRules_;

  State state_;
  TimeStamp newTimeStamp_;
  TimeStamp oldTimeStamp_;

  Node(const Node& other) = delete;
  Node& operator=(const Node&) = delete;
};

/** Class that represents a rule in the graph.
 * A rule is a link between input nodes and output nodes. */
class Rule {
 public:
  /**
   * Construct a rule.
   * @param inputs  Inputs of the rule.
   * @param outputs Outputs of the rule.
   */
  explicit Rule(const NodeArray& inputs, const NodeArray& outputs);

  void addInput(Node* node);
  const NodeArray& getInputs() const;
  NodeArray& getInputs();
  bool isInput(const Node* node) const;

  const NodeArray& getOutputs() const;
  NodeArray& getOutputs();

  bool isPhony() const;
  const std::string& getCommand() const;
  void setCommand(const std::string& cmd);

  const bool hasDepfile() const;
  const std::string& getDepfile() const;
  void setDepfile(const std::string& depfile);

  /* State management */
  State const& getState() const;
  State& getState();
  bool isDirty() const;
  void setState(State state);
  /* Set the state as dirty and mark all the dependencies as dirty too */
  void markDirty();
  /* Set the state as Up to date and mark all the dependencies */
  void markUpToDate();

  void accept(GraphVisitor& v);
 private:
  NodeArray inputs_;
  NodeArray outputs_;

  /** Command to execute in order to generate the outputs. All the inputs must
   * be up-to-date prior to executing the command.
   * Empty string is this is a phony rule. */
  std::string command_;

  /** Path to the file that contains the implicit dependenciess. */
  std::string depfile_;

  /* Set to UP_TO_DATE if all outputs are UP_TO_DATE, OUT_OF_DATE otherwise. */
  State state_;

  Rule(const Rule& other) = delete;
  Rule& operator=(const Rule&) = delete;
};

/**
 * Class that stores a graph of nodes and commands.
 */
class Graph {
 public:
  Graph(const NodeSet& roots, const NodeSet& sources, const NodeMap& nodes,
        const RuleArray& rules);

  void addNode(Node* node);

  const NodeSet& getRoots() const;
  NodeSet& getRoots();

  const NodeSet& getSources() const;
  NodeSet& getSources();

  const NodeMap& getNodes() const;
  NodeMap& getNodes();

  const RuleArray& getRules() const;
  RuleArray& getRules();

  void accept(GraphVisitor& v);

 private:

  /* Contains all the root nodes, ie the nodes that are not an input to any
   * Rule. Typically, 'all' is a root node. */
  NodeSet roots_;

  /* Contains all the leaf nodes, ie the sources. */
  NodeSet sources_;

  /* Contains all the nodes, mapped by their path. */
  NodeMap nodes_;

  /* Contains all the rules */
  RuleArray rules_;

  Graph(const Graph& other) = delete;
  Graph& operator=(const Graph&) = delete;
};

} // namespace falcon


/* ************************************************************************* */
/* Graph visitors */
namespace falcon{

class GraphVisitor {
  public:
    virtual void visit(Graph&) = 0;
    virtual void visit(Node&) = 0;
    virtual void visit(Rule&) = 0;
};

/* A visitor to update the states depending on the node's timestamp */
class GraphTimeStampUpdater : public GraphVisitor {
public:
  virtual void visit(Graph& g);
  virtual void visit(Node& g);
  virtual void visit(Rule& g);
};

/* Graph Printer visitor */

/* A kind of class dedicated to print something or mark something in a stream */
class GraphVisitorPrinter : public GraphVisitor {
public:
  GraphVisitorPrinter(std::ostream& os) : os_(os) {}
protected:
  /* Allow all childs to access this field */
  std::ostream& os_;
private:
  GraphVisitorPrinter() = delete;
};

/* Print a Makefile compatible output */
class GraphMakefilePrinter : public GraphVisitorPrinter {
public:
  /* Use std::cout by default */
  GraphMakefilePrinter();
  GraphMakefilePrinter(std::ostream& os);
  virtual void visit(Graph& g);
  virtual void visit(Node& g);
  virtual void visit(Rule& g);
};

/* Print a graphiz output:
 * the output could be used with dot or graphviz to generate a picture of the
 * graph. */
class GraphGraphizPrinter : public GraphVisitorPrinter {
public:
  /* Use std::cout by default */
  GraphGraphizPrinter();
  GraphGraphizPrinter(std::ostream& os);
  virtual void visit(Graph& g);
  virtual void visit(Node& g);
  virtual void visit(Rule& g);

private:
  /* ******************* */
  /* Rule design options */
  /* edge color: */
  std::string ruleColorOutOfDate_ = "red";
  std::string ruleColorUpToDate_ = "black";

  /* ******************* */
  /* Node design options */
  /* Border color: */
  std::string nodeColorOutOfDate_ = "red";
  std::string nodeColorUpToDate_ = "black";
  /* Fill color: */
  std::string nodeFillColorOutOfDate_ = "white";
  std::string nodeFillColorUpToDate_ = "white";
};

}

#endif // FALCON_GRAPH_H_
