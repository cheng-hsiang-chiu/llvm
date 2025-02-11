//==--------- graph.hpp --- SYCL graph extension ---------------------------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include <CL/sycl/detail/defines_elementary.hpp>

#include <list>
#include <set>
#include <map>

__SYCL_INLINE_NAMESPACE(cl) {
namespace sycl {
namespace ext {
namespace oneapi {
namespace experimental {
namespace detail {

struct node_impl;

struct graph_impl;

using node_ptr = std::shared_ptr<node_impl>;

using graph_ptr = std::shared_ptr<graph_impl>;

class wrapper {
  using T = std::function<void(sycl::handler &)>;
  T my_func;
  std::vector<sycl::event> my_deps;

public:
  wrapper(T t, const std::vector<sycl::event> &deps)
      : my_func(t), my_deps(deps){};

  void operator()(sycl::handler &cgh) {
    cgh.depends_on(my_deps);
    std::invoke(my_func, cgh);
  }
};

struct node_impl {
  bool is_scheduled;
  bool is_empty;

  size_t nid = 0;
  inline void set_nid(const size_t id) { nid = id; }

  graph_ptr my_graph;
  sycl::event my_event;

  std::vector<node_ptr> my_successors;
  std::vector<node_ptr> my_predecessors;

  std::function<void(sycl::handler &)> my_body;

  inline void exec(sycl::queue q) {
    std::vector<sycl::event> __deps;
    std::vector<node_ptr> pred_nodes = my_predecessors;
    while (!pred_nodes.empty()) {
      node_ptr curr_node = pred_nodes.back();
      pred_nodes.pop_back();
      // Add predecessors to dependence list if node is empty
      if (curr_node->is_empty)
        for (auto i : curr_node->my_predecessors)
          pred_nodes.push_back(i);
      else
        __deps.push_back(curr_node->get_event());
    }
    if (my_body && !is_empty) {
      my_event = q.submit(wrapper{my_body, __deps});
    }
  }

  inline void register_successor(node_ptr n) {
    my_successors.push_back(n);
    n->register_predecessor(node_ptr(this));
  }

  inline void register_predecessor(node_ptr n) { my_predecessors.push_back(n); }

  sycl::event get_event(void) { return my_event; }

  node_impl() : is_scheduled(false), is_empty(true) {}

  node_impl(graph_ptr g) : is_scheduled(false), is_empty(true), my_graph(g) {}

  template <typename T>
  node_impl(graph_ptr g, T cgf)
      : is_scheduled(false), is_empty(false), my_graph(g), my_body(cgf) {}

  // Recursively adding nodes to execution stack:
  inline void topology_sort(std::list<node_ptr> &schedule) {
    is_scheduled = true;
    for (auto i : my_successors) {
      if (!i->is_scheduled)
        i->topology_sort(schedule);
    }
    schedule.push_front(node_ptr(this));
  }
};

struct graph_impl {
  std::set<node_ptr> my_roots;
  std::list<node_ptr> my_schedule;

  graph_ptr parent;

  inline void exec(sycl::queue q) {
    if (my_schedule.empty()) {
      for (auto n : my_roots) {
        n->topology_sort(my_schedule);
      }
    }
    for (auto n : my_schedule)
      n->exec(q);
  }

  inline void exec_and_wait(sycl::queue q) {
    exec(q);
    q.wait();
  }

  inline void add_root(node_ptr n) {
    my_roots.insert(n);
    for (auto n : my_schedule)
      n->is_scheduled = false;
    my_schedule.clear();
  }

  inline void remove_root(node_ptr n) {
    my_roots.erase(n);
    for (auto n : my_schedule)
      n->is_scheduled = false;
    my_schedule.clear();
  }

  graph_impl() {}
};

} // namespace detail

class node;

class graph;

class executable_graph;

struct node {
  // TODO: add properties to distinguish between empty, host, device nodes.
  detail::node_ptr my_node;
  detail::graph_ptr my_graph;

  node() : my_node(new detail::node_impl()) {}

  node(detail::graph_ptr g) : my_node(new detail::node_impl(g)), my_graph(g){}

  template <typename T>
  node(detail::graph_ptr g, T cgf)
      : my_node(new detail::node_impl(g, cgf)), my_graph(g){}

  template <typename T> void update(T cgf) {
    my_node->is_scheduled = false;
    my_node->is_empty = false;
    my_node->my_body = cgf;
  }

  inline void register_successor(node n) { my_node->register_successor(n.my_node); }
  inline void exec(sycl::queue q, sycl::event = sycl::event()) {
    my_node->exec(q); }

  inline void set_root() { my_graph->add_root(my_node); }

  // TODO: Add query functions: is_root, ...
};

class executable_graph {
public:
  int my_tag;
  sycl::queue my_queue;

  void exec_and_wait(); // { my_queue.wait(); }

  executable_graph(detail::graph_ptr g, sycl::queue q)
      : my_tag(rand()), my_queue(q) {
    g->exec(my_queue);
  }
};

class graph {
public:
  
  // Adds a node
  template <typename T> node add_node(T cgf, const std::vector<node> &dep = {}, const bool capture=false);

  template <typename T>
  void add_node(node &Node, T cgf, const std::vector<node> &dep = {});

  void add_node(node &Node, const std::vector<node> &dep = {});

  // Adds an empty node
  node add_node(const std::vector<node> &dep = {});

  // Updates a node
  void update_node(node &Node, const std::vector<node> &dep = {});
  template <typename T>
  void update_node(node &Node, T cgf, const std::vector<node> &dep = {});

  // Shortcuts to add graph nodes

  // Adds a fill node
  template <typename T>
  node fill(void *Ptr, const T &Pattern, size_t Count,
            const std::vector<node> &dep = {});
  template <typename T>
  void fill(node &Node, void *Ptr, const T &Pattern, size_t Count,
            const std::vector<node> &dep = {});

  // Adds a memset node
  node memset(void *Ptr, int Value, size_t Count,
              const std::vector<node> &dep = {});
  void memset(node &Node, void *Ptr, int Value, size_t Count,
              const std::vector<node> &dep = {});

  // Adds a memcpy node
  node memcpy(void *Dest, const void *Src, size_t Count,
              const std::vector<node> &dep = {});
  void memcpy(node &Node, void *Dest, const void *Src, size_t Count,
              const std::vector<node> &dep = {});

  // Adds a copy node
  template <typename T>
  node copy(const T *Src, T *Dest, size_t Count,
            const std::vector<node> &dep = {});
  template <typename T>
  void copy(node &Node, const T *Src, T *Dest, size_t Count,
            const std::vector<node> &dep = {});

  // Adds a mem_advise node
  node mem_advise(const void *Ptr, size_t Length, int Advice,
                  const std::vector<node> &dep = {});
  void mem_advise(node &Node, const void *Ptr, size_t Length, int Advice,
                  const std::vector<node> &dep = {});

  // Adds a prefetch node
  node prefetch(const void *Ptr, size_t Count,
                const std::vector<node> &dep = {});
  void prefetch(node &Node, const void *Ptr, size_t Count,
                const std::vector<node> &dep = {});

  // Adds a single_task node
  template <typename KernelName = sycl::detail::auto_name, typename KernelType>
  node single_task(const KernelType &(KernelFunc),
                   const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType>
  void single_task(node &Node, const KernelType &(KernelFunc),
                   const std::vector<node> &dep = {});

  // Adds a parallel_for node
  template <typename KernelName = sycl::detail::auto_name, typename KernelType>
  node parallel_for(range<1> NumWorkItems, const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType>
  void parallel_for(node &Node, range<1> NumWorkItems,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});

  template <typename KernelName = sycl::detail::auto_name, typename KernelType>
  node parallel_for(range<2> NumWorkItems, const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType>
  void parallel_for(node &Node, range<2> NumWorkItems,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});

  template <typename KernelName = sycl::detail::auto_name, typename KernelType>
  node parallel_for(range<3> NumWorkItems, const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType>
  void parallel_for(node &Node, range<3> NumWorkItems,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});

  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims>
  node parallel_for(range<Dims> NumWorkItems, const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims>
  void parallel_for(node &Node, range<Dims> NumWorkItems,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});

  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims>
  node parallel_for(range<Dims> NumWorkItems, id<Dims> WorkItemOffset,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims>
  void parallel_for(node &Node, range<Dims> NumWorkItems,
                    id<Dims> WorkItemOffset, const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});

  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims>
  node parallel_for(nd_range<Dims> ExecutionRange,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims>
  void parallel_for(node &Node, nd_range<Dims> ExecutionRange,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});

  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims, typename Reduction>
  node parallel_for(range<Dims> NumWorkItems, Reduction Redu,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims, typename Reduction>
  void parallel_for(node &Node, range<Dims> NumWorkItems, Reduction Redu,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});

  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims, typename Reduction>
  node parallel_for(nd_range<Dims> ExecutionRange, Reduction Redu,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});
  template <typename KernelName = sycl::detail::auto_name, typename KernelType,
            int Dims, typename Reduction>
  void parallel_for(node &Node, nd_range<Dims> ExecutionRange, Reduction Redu,
                    const KernelType &(KernelFunc),
                    const std::vector<node> &dep = {});

  // Adds a dependency between two nodes.
  void make_edge(node sender, node receiver);

  // TODO: Extend queue to directly submit graph
  void exec_and_wait(sycl::queue q);

  inline executable_graph instantiate(sycl::queue q) {
    return executable_graph{my_graph, q};
  };

  graph() : my_graph(new detail::graph_impl()), ptr_prev_node(nullptr) {}

  // Creates a subgraph (with predecessors)
  graph(graph &parent, const std::vector<node> &dep = {}) {}

  bool is_subgraph();

  // Returns the number of nodes in the graph
  size_t num_nodes() const;

  // Returns the number of edges in the graph
  size_t num_edges() const;

  // The unique id of a node
  size_t uid = 0;

  // Returns the uid of a node
  size_t get_id() const;

  // Updates the uid of a node
  void set_id(const size_t);

  // The map between event id and node id
  std::map<size_t, node> id2node;

  detail::node_ptr locate_node(const size_t);

private:
  detail::graph_ptr my_graph;
  detail::node_ptr ptr_prev_node;
};

inline void executable_graph::exec_and_wait() {
  my_queue.wait();
}

/// Adds a node to the graph, in order to be executed upon graph execution.
///
/// \param cgf is a function object containing command group.
/// \param dep is a vector of graph nodes the to be added node depends on.
/// \param capture is a variable denoting if being in a capture mode.
/// \return a graph node representing the command group operation.
template <typename T>
inline node graph::add_node(T cgf, const std::vector<node> &dep, const bool capture) {
  node _node(my_graph, cgf);
  if (!capture) {
    if (!dep.empty()) {
      for (auto n : dep)
        this->make_edge(n, _node);
    } else {
      _node.set_root();
    }
  }
  else {
    // first node ever
    if (!ptr_prev_node) {
      _node.set_root();
      ptr_prev_node = _node.my_node;
    }
    else {
      ptr_prev_node->register_successor(_node.my_node);
      ptr_prev_node = _node.my_node;
    }
  }
  return _node;
}

/// Adds an empty node to the graph, in order to be executed upon graph
/// execution.
///
/// \param dep is a vector of graph nodes the to be added node depends on.
/// \return a graph node representing no operations but potentially node
/// dependencies.
inline node graph::add_node(const std::vector<node> &dep) {
  node _node(my_graph);
  if (!dep.empty()) {
    for (auto n : dep)
      this->make_edge(n, _node);
  } else {
    _node.set_root();
  }
  return _node;
}

/// Adds a node to the graph, in order to be executed upon graph execution.
///
/// \param Node is the graph node to be used. This overwrites the node
/// parameters.
/// \param dep is a vector of graph nodes the to be added node depends on.
inline void graph::add_node(node &Node, const std::vector<node> &dep) {
  Node.my_graph = this->my_graph;
  Node.my_node->my_graph = this->my_graph;
  Node.my_node->is_empty = false;
  if (!dep.empty()) {
    for (auto n : dep)
      this->make_edge(n, Node);
  } else {
    Node.set_root();
  }
}

/// Adds a node to the graph, in order to be executed upon graph execution.
///
/// \param Node is the graph node to be used. This overwrites the node
/// parameters.
/// \param cgf is a function object containing command group.
/// \param dep is a vector of graph nodes the to be added node depends on.
template <typename T>
inline void graph::add_node(node &Node, T cgf, const std::vector<node> &dep) {
  Node.my_graph = this->my_graph;
  Node.my_node->my_graph = this->my_graph;
  Node.my_node->my_body = cgf;
  Node.my_node->is_empty = false;
  if (!dep.empty()) {
    for (auto n : dep)
      this->make_edge(n, Node);
  } else {
    Node.set_root();
  }
}

/// Sets or updates a graph node by overwriting its dependencies.
///
/// \param Node is a graph node to be updated.
/// \param dep is a vector of graph nodes the to be updated node depends on.
inline void graph::update_node(node &Node, const std::vector<node> &dep) {
  Node.my_graph = this->my_graph;
  Node.my_node->my_graph = this->my_graph;
  Node.my_node->is_empty = true;
  if (!dep.empty()) {
    for (auto n : dep)
      this->make_edge(n, Node);
  } else {
    Node.set_root();
  }
}

/// Sets or updates a graph node by overwriting its parameters.
///
/// \param Node is a graph node to be updated.
/// \param cgf is a function object containing command group.
/// \param dep is a vector of graph nodes the to be updated node depends on.
template <typename T>
inline void graph::update_node(node &Node, T cgf, const std::vector<node> &dep) {
  Node.my_graph = this->my_graph;
  Node.my_node->my_graph = this->my_graph;
  Node.my_node->my_body = cgf;
  Node.my_node->is_empty = false;
  if (!dep.empty()) {
    for (auto n : dep)
      this->make_edge(n, Node);
  } else {
    Node.set_root();
  }
}

/// Fills the specified memory with the specified pattern.
///
/// \param Ptr is the pointer to the memory to fill.
/// \param Pattern is the pattern to fill into the memory.  T should be
/// trivially copyable.
/// \param Count is the number of times to fill Pattern into Ptr.
/// \param dep is a vector of graph nodes the fill depends on.
/// \return a graph node representing the fill operation.
template <typename T>
inline node graph::fill(void *Ptr, const T &Pattern, size_t Count,
                 const std::vector<node> &dep) {
  return graph::add_node([=](sycl::handler &h) { h.fill(Ptr, Pattern, Count); },
                         dep);
}

/// Fills the specified memory with the specified pattern.
///
/// \param Node is the graph node to be used for the fill. This overwrites
/// the node parameters.
/// \param Ptr is the pointer to the memory to fill.
/// \param Pattern is the pattern to fill into the memory.  T should be
/// trivially copyable.
/// \param Count is the number of times to fill Pattern into Ptr.
/// \param dep is a vector of graph nodes the fill depends on.
template <typename T>
inline void graph::fill(node &Node, void *Ptr, const T &Pattern, size_t Count,
                 const std::vector<node> &dep) {
  graph::update_node(
      Node, [=](sycl::handler &h) { h.fill(Ptr, Pattern, Count); }, dep);
}

/// Copies data from one memory region to another, both pointed by
/// USM pointers.
/// No operations is done if \param Count is zero. An exception is thrown
/// if either \param Dest or \param Src is nullptr. The behavior is undefined
/// if any of the pointer parameters is invalid.
///
/// \param Dest is a USM pointer to the destination memory.
/// \param Src is a USM pointer to the source memory.
/// \param dep is a vector of graph nodes the memset depends on.
/// \return a graph node representing the memset operation.
inline node graph::memset(void *Ptr, int Value, size_t Count,
                   const std::vector<node> &dep) {
  return graph::add_node([=](sycl::handler &h) { h.memset(Ptr, Value, Count); },
                         dep);
}

/// Copies data from one memory region to another, both pointed by
/// USM pointers.
/// No operations is done if \param Count is zero. An exception is thrown
/// if either \param Dest or \param Src is nullptr. The behavior is undefined
/// if any of the pointer parameters is invalid.
///
/// \param Node is the graph node to be used for the memset. This overwrites
/// the node parameters.
/// \param Dest is a USM pointer to the destination memory.
/// \param Src is a USM pointer to the source memory.
/// \param dep is a vector of graph nodes the memset depends on.
inline void graph::memset(node &Node, void *Ptr, int Value, size_t Count,
                   const std::vector<node> &dep) {
  graph::update_node(
      Node, [=](sycl::handler &h) { h.memset(Ptr, Value, Count); }, dep);
}

/// Copies data from one memory region to another, both pointed by
/// USM pointers.
/// No operations is done if \param Count is zero. An exception is thrown
/// if either \param Dest or \param Src is nullptr. The behavior is undefined
/// if any of the pointer parameters is invalid.
///
/// \param Dest is a USM pointer to the destination memory.
/// \param Src is a USM pointer to the source memory.
/// \param Count is a number of bytes to copy.
/// \param dep is a vector of graph nodes the memcpy depends on.
/// \return a graph node representing the memcpy operation.
inline node graph::memcpy(void *Dest, const void *Src, size_t Count,
                   const std::vector<node> &dep) {
  return graph::add_node([=](sycl::handler &h) { h.memcpy(Dest, Src, Count); },
                         dep);
}

/// Copies data from one memory region to another, both pointed by
/// USM pointers.
/// No operations is done if \param Count is zero. An exception is thrown
/// if either \param Dest or \param Src is nullptr. The behavior is undefined
/// if any of the pointer parameters is invalid.
///
/// \param Node is the graph node to be used for the memcpy. This overwrites
/// the node parameters.
/// \param Dest is a USM pointer to the destination memory.
/// \param Src is a USM pointer to the source memory.
/// \param Count is a number of bytes to copy.
/// \param dep is a vector of graph nodes the memcpy depends on.
inline void graph::memcpy(node &Node, void *Dest, const void *Src, size_t Count,
                   const std::vector<node> &dep) {
  graph::update_node(
      Node, [=](sycl::handler &h) { h.memcpy(Dest, Src, Count); }, dep);
}

/// Copies data from one memory region to another, both pointed by
/// USM pointers.
/// No operations is done if \param Count is zero. An exception is thrown
/// if either \param Dest or \param Src is nullptr. The behavior is undefined
/// if any of the pointer parameters is invalid.
///
/// \param Src is a USM pointer to the source memory.
/// \param Dest is a USM pointer to the destination memory.
/// \param Count is a number of elements of type T to copy.
/// \param dep is a vector of graph nodes the copy depends on.
/// \return a graph node representing the copy operation.
template <typename T>
inline node graph::copy(const T *Src, T *Dest, size_t Count,
                 const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) { h.memcpy(Dest, Src, Count * sizeof(T)); }, dep);
}

/// Copies data from one memory region to another, both pointed by
/// USM pointers.
/// No operations is done if \param Count is zero. An exception is thrown
/// if either \param Dest or \param Src is nullptr. The behavior is undefined
/// if any of the pointer parameters is invalid.
///
/// \param Node is the graph node to be used for the copy. This overwrites
/// the node parameters.
/// \param Src is a USM pointer to the source memory.
/// \param Dest is a USM pointer to the destination memory.
/// \param Count is a number of elements of type T to copy.
/// \param dep is a vector of graph nodes the copy depends on.
template <typename T>
inline void graph::copy(node &Node, const T *Src, T *Dest, size_t Count,
                 const std::vector<node> &dep) {
  graph::update_node(
      Node, [=](sycl::handler &h) { h.memcpy(Dest, Src, Count * sizeof(T)); },
      dep);
}

/// Provides additional information to the underlying runtime about how
/// different allocations are used.
///
/// \param Ptr is a USM pointer to the allocation.
/// \param Length is a number of bytes in the allocation.
/// \param Advice is a device-defined advice for the specified allocation.
/// \param dep is a vector of graph nodes the mem_advise depends on.
/// \return a graph node representing the mem_advise operation.
inline node graph::mem_advise(const void *Ptr, size_t Length, int Advice,
                       const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) { h.mem_advise(Ptr, Length, Advice); }, dep);
}

/// Provides additional information to the underlying runtime about how
/// different allocations are used.
///
/// \param Node is the graph node to be used for the mem_advise. This overwrites
/// the node parameters.
/// \param Ptr is a USM pointer to the allocation.
/// \param Length is a number of bytes in the allocation.
/// \param Advice is a device-defined advice for the specified allocation.
/// \param dep is a vector of graph nodes the mem_advise depends on.
inline void graph::mem_advise(node &Node, const void *Ptr, size_t Length, int Advice,
                       const std::vector<node> &dep) {
  graph::update_node(
      Node, [=](sycl::handler &h) { h.mem_advise(Ptr, Length, Advice); }, dep);
}

/// Provides hints to the runtime library that data should be made available
/// on a device earlier than Unified Shared Memory would normally require it
/// to be available.
///
/// \param Ptr is a USM pointer to the memory to be prefetched to the device.
/// \param Count is a number of bytes to be prefetched.
/// \param dep is a vector of graph nodes the prefetch depends on.
/// \return a graph node representing the prefetch operation.
inline node graph::prefetch(const void *Ptr, size_t Count,
                     const std::vector<node> &dep) {
  return graph::add_node([=](sycl::handler &h) { h.prefetch(Ptr, Count); },
                         dep);
}

/// Provides hints to the runtime library that data should be made available
/// on a device earlier than Unified Shared Memory would normally require it
/// to be available.
///
/// \param Node is the graph node to be used for the prefetch. This overwrites
/// the node parameters.
/// \param Ptr is a USM pointer to the memory to be prefetched to the device.
/// \param Count is a number of bytes to be prefetched.
/// \param dep is a vector of graph nodes the prefetch depends on.
inline void graph::prefetch(node &Node, const void *Ptr, size_t Count,
                     const std::vector<node> &dep) {
  graph::update_node(
      Node, [=](sycl::handler &h) { h.prefetch(Ptr, Count); }, dep);
}

/// single_task version with a kernel represented as a lambda.
///
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the single_task depends on.
/// \return a graph node representing the single_task operation.
template <typename KernelName, typename KernelType>
inline node graph::single_task(const KernelType &(KernelFunc),
                        const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template single_task<KernelName, KernelType>(KernelFunc);
      },
      dep);
}

/// single_task version with a kernel represented as a lambda.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the single_task depends on.
template <typename KernelName, typename KernelType>
inline void graph::single_task(node &Node, const KernelType &(KernelFunc),
                        const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template single_task<KernelName, KernelType>(KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global size only.
///
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
/// \return a graph node representing the parallel_for operation.
template <typename KernelName, typename KernelType>
inline node graph::parallel_for(range<1> NumWorkItems, const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(NumWorkItems,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global size only.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
template <typename KernelName, typename KernelType>
inline void graph::parallel_for(node &Node, range<1> NumWorkItems,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(NumWorkItems,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global size only.
///
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
/// \return a graph node representing the parallel_for operation.
template <typename KernelName, typename KernelType>
inline node graph::parallel_for(range<2> NumWorkItems, const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(NumWorkItems,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global size only.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
template <typename KernelName, typename KernelType>
inline void graph::parallel_for(node &Node, range<2> NumWorkItems,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(NumWorkItems,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global size only.
///
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
/// \return a graph node representing the parallel_for operation.
template <typename KernelName, typename KernelType>
inline node graph::parallel_for(range<3> NumWorkItems, const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(NumWorkItems,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global size only.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
template <typename KernelName, typename KernelType>
inline void graph::parallel_for(node &Node, range<3> NumWorkItems,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(NumWorkItems,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global size only.
///
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
/// \return a graph node representing the parallel_for operation.
template <typename KernelName, typename KernelType, int Dims>
inline node graph::parallel_for(range<Dims> NumWorkItems,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(NumWorkItems,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global size only.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
template <typename KernelName, typename KernelType, int Dims>
inline void graph::parallel_for(node &Node, range<Dims> NumWorkItems,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(NumWorkItems,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range and
/// offset that specify global size and global offset correspondingly.
///
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param WorkItemOffset specifies the offset for each work item id
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
/// \return a graph node representing the parallel_for operation.
template <typename KernelName, typename KernelType, int Dims>
inline node graph::parallel_for(range<Dims> NumWorkItems, id<Dims> WorkItemOffset,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(
            NumWorkItems, WorkItemOffset, KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range and
/// offset that specify global size and global offset correspondingly.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param WorkItemOffset specifies the offset for each work item id
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
template <typename KernelName, typename KernelType, int Dims>
inline void graph::parallel_for(node &Node, range<Dims> NumWorkItems,
                         id<Dims> WorkItemOffset,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(
            NumWorkItems, WorkItemOffset, KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + nd_range that
/// specifies global, local sizes and offset.
///
/// \param ExecutionRange is a range that specifies the work space of the
/// kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
/// \return a graph node representing the parallel_for operation.
template <typename KernelName, typename KernelType, int Dims>
inline node graph::parallel_for(nd_range<Dims> ExecutionRange,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(ExecutionRange,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + nd_range that
/// specifies global, local sizes and offset.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param ExecutionRange is a range that specifies the work space of the
/// kernel
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
template <typename KernelName, typename KernelType, int Dims>
inline void graph::parallel_for(node &Node, nd_range<Dims> ExecutionRange,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType>(ExecutionRange,
                                                        KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global, local sizes and offset.
///
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param Redu is a reduction operation
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
/// \return a graph node representing the parallel_for operation.
template <typename KernelName, typename KernelType, int Dims,
          typename Reduction>
inline node graph::parallel_for(range<Dims> NumWorkItems, Reduction Redu,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType, Dims, Reduction>(
            NumWorkItems, Redu, KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + range that
/// specifies global, local sizes and offset.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param NumWorkItems is a range that specifies the work space of the kernel
/// \param Redu is a reduction operation
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
template <typename KernelName, typename KernelType, int Dims,
          typename Reduction>
inline void graph::parallel_for(node &Node, range<Dims> NumWorkItems, Reduction Redu,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType, Dims, Reduction>(
            NumWorkItems, Redu, KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + nd_range that
/// specifies global, local sizes and offset.
///
/// \param ExecutionRange is a range that specifies the work space of the
/// kernel
/// \param Redu is a reduction operation
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
/// \return a graph node representing the parallel_for operation.
template <typename KernelName, typename KernelType, int Dims,
          typename Reduction>
inline node graph::parallel_for(nd_range<Dims> ExecutionRange, Reduction Redu,
                         const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  return graph::add_node(
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType, Dims, Reduction>(
            ExecutionRange, Redu, KernelFunc);
      },
      dep);
}

/// parallel_for version with a kernel represented as a lambda + nd_range that
/// specifies global, local sizes and offset.
///
/// \param Node is the graph node to be used for the single_task. This
/// overwrites the node parameters.
/// \param ExecutionRange is a range that specifies the work space of the
/// kernel
/// \param Redu is a reduction operation
/// \param KernelFunc is the Kernel functor or lambda
/// \param dep is a vector of graph nodes the parallel_for depends on
template <typename KernelName, typename KernelType, int Dims,
          typename Reduction>
inline void graph::parallel_for(node &Node, nd_range<Dims> ExecutionRange,
                         Reduction Redu, const KernelType &(KernelFunc),
                         const std::vector<node> &dep) {
  graph::update_node(
      Node,
      [=](sycl::handler &h) {
        h.template parallel_for<KernelName, KernelType, Dims, Reduction>(
            ExecutionRange, Redu, KernelFunc);
      },
      dep);
}

inline void graph::make_edge(node sender, node receiver) {
  sender.register_successor(receiver);     // register successor
  my_graph->remove_root(receiver.my_node); // remove receiver from root node
                                           // list
}

inline void graph::exec_and_wait(sycl::queue q) { my_graph->exec_and_wait(q); }

inline size_t graph::num_nodes() const {
  return my_graph->my_schedule.size();
}

inline size_t graph::num_edges() const {
  size_t num_edges = 0;
  for (auto& root: my_graph->my_roots) {
    num_edges += root->my_successors.size();
  }
  return num_edges;
}

inline size_t graph::get_id() const {
  return uid;
}

inline void graph::set_id(const size_t id) {
  uid = id;
}

inline detail::node_ptr graph::locate_node(const size_t id) {
  for (auto root : my_graph->my_roots) {
    if (root->nid == id) {
      return root;
    }
    else {
      for (auto successor : root->my_successors) {
        if (successor->nid == id) {
          return successor;
        }
      }
    }
  }
}

} // namespace experimental
} // namespace oneapi
} // namespace ext


} // namespace sycl
} // __SYCL_INLINE_NAMESPACE(cl)
